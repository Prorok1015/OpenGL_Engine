#include "edt_decoded_model_adapter.h"
#include "edt_process_decoder.h"
#include <snk/common/fs_paths.h>
#include <snk/resource/res_path_sanitize.h>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

// The tag of a file that is still there afterwards, if any.
std::optional<res::tag> erase_memory_folder(res::resource_system& resources, const std::string& folder)
{
	const std::vector<fs::path> roots = resources.mount_roots(res::tag::memory);
	if (roots.empty()) return std::nullopt;
	const fs::path dir = roots.back() / fs_paths::path_from_utf8(folder);
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return std::nullopt;
	std::vector<res::tag> found;
	for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
		if (it->is_regular_file(ec)) found.emplace_back(res::tag::memory, folder + fs_paths::generic_utf8_of(it->path().lexically_relative(dir)));
	}
	for (const res::tag& tag : found) resources.erase(tag);
	for (const res::tag& tag : found) {
		if (resources.exists(tag)) return tag;
	}
	return std::nullopt;
}

}

edt::decoded_model_adapter::decoded_model_adapter(desc::desc_system& descs, res::resource_system& resources, decoder_steps decoder)
	: m_descs(descs)
	, m_resources(resources)
	, m_decoder(std::move(decoder))
{
}

edt::decoded_model_adapter::decoder_steps edt::decoded_model_adapter::steps_of(std::shared_ptr<const process_decoder> decoder)
{
	decoder_steps steps;
	steps.scan = [decoder](const import_source& source, ds::task_context* task) { return decoder->scan(source, task); };
	steps.import = [decoder](const import_source& source, ds::task_context* task) { return decoder->import(source, task); };
	steps.unavailable_reason = [decoder] { return decoder->unavailable_reason(); };
	return steps;
}

std::optional<edt::model_location> edt::decoded_model_adapter::locate(const std::vector<fs::path>& roots, const res::tag& tag)
{
	for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
		const std::optional<fs::path> file = res::path_within_dir(*root, tag.relative());
		std::error_code ec;
		if (!file || !fs::is_regular_file(*file, ec)) continue;
		fs::path canonical_root = fs::weakly_canonical(*root, ec);
		if (ec) canonical_root = root->lexically_normal();
		return model_location{ *file, std::move(canonical_root) };
	}
	return std::nullopt;
}

std::string edt::decoded_model_adapter::memory_folder_of(const res::tag& tag)
{
	const std::string below = res::sanitize_relative_path(std::string(tag.path()) + std::string(tag.name()));
	return "__models/" + std::string(tag.protocol()) + "/" + below + "/";
}

std::shared_ptr<res::resource_entry> edt::decoded_model_adapter::deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const
{
	const auto refuse = [&tag](std::string_view why) {
		return std::runtime_error(std::format("model '{}' is not loaded: {}", tag.view(), why));
	};
	if (const std::string why = m_decoder.unavailable_reason ? m_decoder.unavailable_reason() : std::string{}; !why.empty()) {
		throw refuse(why);
	}
	if (raw_data.size() > k_max_person_source_bytes) {
		throw refuse(std::format("the file is too big: {} bytes, the limit is {}", raw_data.size(), k_max_person_source_bytes));
	}

	import_source source;
	source.preloaded = &raw_data;
	source.memory_folder = memory_folder_of(tag);
	if (const std::optional<model_location> location = locate(m_resources.mount_roots(tag.protocol()), tag)) {
		source.path = location->file;
		source.boundary = location->root;
	} else {
		// Served from memory or a pack: the decoder gets only the bytes, and the model may name no other file.
		source.path = fs_paths::path_from_utf8(source.memory_folder) / fs_paths::path_from_utf8(tag.name());
		source.on_disk = false;
	}

	// The decoder's own errors (it failed, was killed, its answer was refused) name the model too.
	const auto step = [&refuse](auto&& call) {
		try {
			return call();
		} catch (const std::runtime_error& e) {
			throw refuse(e.what());
		}
	};
	source.deadline = std::chrono::steady_clock::now() + k_deadline;
	const model_file_scan found = step([&] { return m_decoder.scan(source, nullptr); });
	if (!found.ok()) throw refuse(found.error);
	source.allowed = &found.files;

	const std::shared_ptr<std::mutex> lock = folder_lock(source.memory_folder);
	const std::lock_guard folder_guard(*lock);
	if (const std::optional<res::tag> left = erase_memory_folder(m_resources, source.memory_folder)) {
		throw refuse(std::format("'{}', left in memory:// by an earlier load of the model, cannot be removed", left->view()));
	}
	source.deadline = std::chrono::steady_clock::now() + k_deadline;
	import_intermediate imported = step([&] { return m_decoder.import(source, nullptr); });
	if (!imported.error.empty()) throw refuse(imported.error);

	std::shared_ptr<desc::desc_base> instance = m_descs.create_instance("prefab_desc", imported.prefab_tag);
	if (!instance) throw refuse("the prefab of the model cannot be made");
	instance->deserialize(m_descs, imported.prefab_json);
	m_resources.register_alias(tag, imported.prefab_tag);
	return instance;
}

std::shared_ptr<std::mutex> edt::decoded_model_adapter::folder_lock(const std::string& folder) const
{
	const std::lock_guard guard(m_folder_locks_mutex);
	std::shared_ptr<std::mutex>& lock = m_folder_locks[folder];
	if (!lock) lock = std::make_shared<std::mutex>();
	return lock;
}

std::vector<std::byte> edt::decoded_model_adapter::serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>&) const
{
	throw std::logic_error(std::format("model '{}' cannot be written: models are read only", tag.view()));
}
