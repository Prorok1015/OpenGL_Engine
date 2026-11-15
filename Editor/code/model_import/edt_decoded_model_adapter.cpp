#include "edt_decoded_model_adapter.h"
#include "edt_process_decoder.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_path_sanitize.h>
#include <cstdint>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

// The bytes of the model and what decides how they decode. The side files are not in it: a hit reads again only the files
// the scan listed, so a file the model names that was missing at the first load is not looked for until the bytes change.
std::string cache_key(const std::string& decoder_version, const std::vector<std::byte>& model, const edt::import_source& source)
{
	return std::format("{}|{}|{}/{}/{}|{}|{}|{}", decoder_version, edt::digest_to_hex(edt::digest_of(model)),
		source.limits.max_total_bytes, source.limits.max_file_bytes, source.limits.max_files, source.on_disk ? 1 : 0,
		fs_paths::generic_utf8_of(source.boundary), fs_paths::generic_utf8_of(source.path));
}

// The whole file, when it has exactly `size` bytes.
std::optional<std::vector<std::byte>> read_exactly(const fs::path& path, std::uintmax_t size)
{
	std::error_code ec;
	const std::uintmax_t on_disk = fs::file_size(path, ec);
	if (ec || on_disk != size) return std::nullopt;
	std::ifstream in(path, std::ios::binary);
	if (!in) return std::nullopt;
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
	return bytes;
}

// What process_decoder::import asks the decoder for `source`.
edt::decode_request import_request(const edt::import_source& source, const std::vector<edt::referenced_file>& allowed)
{
	edt::decode_request request;
	request.mode = edt::decode_request::mode_kind::import;
	request.source_path = source.path;
	request.allowed = allowed;
	request.limits = source.limits;
	request.boundary = source.boundary;
	request.memory_folder = source.memory_folder;
	request.on_disk = source.on_disk;
	return request;
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
	return edt::steps_of(std::move(decoder));
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
	const std::string version = m_decoder.version ? m_decoder.version() : std::string{};
	const std::string key = version.empty() ? std::string{} : cache_key(version, raw_data, source);

	// Held through the scan too: a second load of the model waits for the first and finds its import.
	const std::shared_ptr<std::mutex> lock = folder_lock(source.memory_folder);
	const std::lock_guard folder_guard(*lock);
	std::optional<import_intermediate> imported;
	if (!key.empty()) {
		std::string problem;
		imported = cached(key, source, problem);
		if (!problem.empty()) {
			egLOG_WARN("edt/decoder", "the earlier decode of '{}' was refused, it is decoded again: {}", tag.view(), problem);
		}
	}
	if (!imported) {
		forget(source.memory_folder);
		source.deadline = std::chrono::steady_clock::now() + k_deadline;
		const model_file_scan found = step([&] { return m_decoder.scan(source, nullptr); });
		if (!found.ok()) throw refuse(found.error);
		source.allowed = &found.files;

		if (const std::optional<res::tag> left = erase_memory_folder(m_resources, source.memory_folder)) {
			throw refuse(std::format("'{}', left in memory:// by an earlier load of the model, cannot be removed", left->view()));
		}
		source.deadline = std::chrono::steady_clock::now() + k_deadline;
		imported = step([&] { return m_decoder.import(source, nullptr); });
		if (!imported->error.empty()) throw refuse(imported->error);
		if (!key.empty()) remember(source.memory_folder, cached_decode{ key, found, *imported });
	}

	std::shared_ptr<desc::desc_base> instance = m_descs.create_instance("prefab_desc", imported->prefab_tag);
	if (!instance) throw refuse("the prefab of the model cannot be made");
	instance->deserialize(m_descs, imported->prefab_json);
	m_resources.register_alias(tag, imported->prefab_tag);
	return instance;
}

std::optional<edt::import_intermediate> edt::decoded_model_adapter::cached(const std::string& key, const import_source& source, std::string& problem) const
{
	cached_decode decode;
	{
		const std::lock_guard guard(m_cache_mutex);
		const auto found = m_cache.find(source.memory_folder);
		if (found == m_cache.end() || found->second.key != key) return std::nullopt;
		decode = found->second;
	}
	// A side file that changed (or is gone) is a new model to decode, not a refusal.
	for (const referenced_file& file : decode.scan.files) {
		const std::optional<std::vector<std::byte>> bytes = read_exactly(file.path, file.size);
		if (!bytes || !file.hashed || digest_of(*bytes) != file.digest) return std::nullopt;
	}

	// What is in memory:// now is read back as a decoder's answer, and checked as one: anyone may change those files.
	const std::vector<fs::path> roots = m_resources.mount_roots(res::tag::memory);
	if (roots.empty()) return std::nullopt;
	const decode_request request = import_request(source, decode.scan.files);
	const std::uintmax_t blob_limit = static_cast<std::uintmax_t>(request.limits.max_file_bytes) * 2;
	decode_bundle answer;
	answer.manifest["kind"] = "import";
	answer.manifest["import"] = to_json(decode.imported);
	for (const res::tag& resource : decode.imported.created_tags) {
		const std::optional<fs::path> file = res::path_within_dir(roots.back(), resource.relative());
		std::error_code ec;
		const std::uintmax_t size = file ? fs::file_size(*file, ec) : 0;
		// Gone (erased, or the folder was swept): decoded again like a model seen for the first time.
		if (!file || ec) return std::nullopt;
		if (size > blob_limit) {
			problem = std::format("'{}' is bigger than a resource of the import may be", resource.view());
			return std::nullopt;
		}
		std::optional<std::vector<std::byte>> bytes = read_exactly(*file, size);
		if (!bytes) return std::nullopt;
		answer.blobs.push_back(bundle_blob{ std::string(resource.view()), std::move(*bytes) });
	}
	return accept_import_answer(answer, request, problem);
}

void edt::decoded_model_adapter::remember(const std::string& folder, cached_decode decode) const
{
	const std::lock_guard guard(m_cache_mutex);
	m_cache.insert_or_assign(folder, std::move(decode));
}

void edt::decoded_model_adapter::forget(const std::string& folder) const
{
	const std::lock_guard guard(m_cache_mutex);
	m_cache.erase(folder);
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
