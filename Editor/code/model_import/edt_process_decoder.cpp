#include "edt_process_decoder.h"
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <algorithm>
#include <format>
#include <iterator>
#include <stdexcept>
#include <stop_token>
#include <unordered_set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t k_mib = 1024ull * 1024ull;
constexpr std::uint64_t k_default_memory_mb = 2048;
constexpr std::uint64_t k_min_memory_mb = 256;
constexpr std::uint64_t k_max_memory_mb = 8192;
constexpr std::size_t k_max_error_bytes = 300;

// What the child says about itself goes to an agent and into a log: cut, and without anything a terminal acts on.
std::string cleaned(std::string_view text)
{
	std::string out;
	for (const char c : text.substr(0, k_max_error_bytes)) {
		const auto byte = static_cast<unsigned char>(c);
		out.push_back((byte < 0x20 || byte == 0x7f) ? ' ' : c);
	}
	if (out == "std::bad_alloc") return "the decoder ran out of memory";
	return out;
}

const boost::json::string* string_of(const boost::json::object& object, const char* key)
{
	const boost::json::value* value = object.if_contains(key);
	return value != nullptr && value->is_string() ? &value->as_string() : nullptr;
}

// True when `path` lies in `folder` (lexically; both are as the editor resolved them) and has no ".." in it.
bool lies_in(const fs::path& path, const fs::path& folder)
{
	const fs::path normal = path.lexically_normal();
	if (!normal.is_absolute()) return false;
	for (const fs::path& part : normal) {
		if (part == "..") return false;
	}
	const fs::path root = folder.lexically_normal();
	const auto mismatch = std::mismatch(root.begin(), root.end(), normal.begin(), normal.end());
	// `root` may end with an empty element ("dir/"): the rest of it is then the empty tail.
	return mismatch.first == root.end() || (std::next(mismatch.first) == root.end() && mismatch.first->empty());
}

// "memory://<name>/..." -> "memory://<name>/", empty when the tag has no folder.
std::string folder_prefix_of(const std::string& tag)
{
	const std::string_view scheme = "memory://";
	if (!tag.starts_with(scheme)) return {};
	const std::size_t slash = tag.find('/', scheme.size());
	return slash == std::string::npos ? std::string{} : tag.substr(0, slash + 1);
}

} // namespace

std::optional<edt::model_file_scan> edt::accept_scan_answer(const decode_bundle& bundle, const decode_request& request, std::string& problem)
{
	const boost::json::object& manifest = bundle.manifest;
	const boost::json::string* kind = string_of(manifest, "kind");
	if (kind == nullptr || *kind != "scan") {
		problem = "the answer is not the answer to a scan";
		return std::nullopt;
	}
	if (!bundle.blobs.empty()) {
		problem = "a scan came with resources";
		return std::nullopt;
	}
	if (const boost::json::string* error = string_of(manifest, "error")) {
		model_file_scan failed;
		failed.error = cleaned(*error);
		if (failed.error.empty()) failed.error = "the model could not be read";
		return failed;
	}
	const boost::json::value* found = manifest.if_contains("scan");
	if (found == nullptr) {
		problem = "the answer to a scan holds no scan";
		return std::nullopt;
	}
	std::optional<model_file_scan> scan = model_file_scan_from_json(*found);
	if (!scan) {
		problem = "the answer to a scan cannot be read";
		return std::nullopt;
	}
	scan->error = cleaned(scan->error);
	if (!scan->ok()) return scan;

	const fs::path folder = request.source_path.parent_path();
	if (scan->files.size() > request.limits.max_files) {
		problem = "the scan lists more files than the limit allows";
		return std::nullopt;
	}
	std::uintmax_t total = 0;
	std::unordered_set<std::string> seen;
	for (const referenced_file& file : scan->files) {
		if (!lies_in(file.path, folder)) {
			problem = "the scan lists a file outside the folder of the model";
			return std::nullopt;
		}
		if (file.size > request.limits.max_file_bytes || (total += file.size) > request.limits.max_total_bytes) {
			problem = "the scan lists more bytes than the limits allow";
			return std::nullopt;
		}
		if (!seen.insert(file.path.lexically_normal().generic_string()).second) {
			problem = "the scan lists a file twice";
			return std::nullopt;
		}
	}
	scan->total_bytes = total;
	return scan;
}

std::optional<edt::import_intermediate> edt::accept_import_answer(const decode_bundle& bundle, const decode_request& request, std::string& problem)
{
	const boost::json::object& manifest = bundle.manifest;
	const boost::json::string* kind = string_of(manifest, "kind");
	if (kind == nullptr || *kind != "import") {
		problem = "the answer is not the answer to an import";
		return std::nullopt;
	}
	if (const boost::json::string* error = string_of(manifest, "error")) {
		if (!bundle.blobs.empty()) {
			problem = "a failed import came with resources";
			return std::nullopt;
		}
		import_intermediate failed;
		failed.error = cleaned(*error);
		if (failed.error.empty()) failed.error = "the model could not be imported";
		failed.source_path = request.source_path;
		return failed;
	}
	const boost::json::value* found = manifest.if_contains("import");
	if (found == nullptr) {
		problem = "the answer to an import holds no import";
		return std::nullopt;
	}
	std::optional<import_intermediate> imported = import_intermediate_from_json(*found);
	if (!imported) {
		problem = "the answer to an import cannot be read";
		return std::nullopt;
	}
	imported->source_path = request.source_path;
	imported->error = cleaned(imported->error);
	if (!imported->error.empty()) {
		if (!bundle.blobs.empty()) {
			problem = "a failed import came with resources";
			return std::nullopt;
		}
		return imported;
	}

	const std::string prefix = folder_prefix_of(std::string(imported->prefab_tag.view()));
	if (prefix.empty()) {
		problem = "the prefab of the import is not below a memory:// folder";
		return std::nullopt;
	}
	const auto below_prefix = [&prefix](const res::tag& tag) {
		return is_bundle_tag(std::string(tag.view()), bundle_limits{}.max_tag_bytes) && std::string(tag.view()).starts_with(prefix);
	};
	std::unordered_set<std::string> all;
	for (const res::tag& tag : imported->all_tags) {
		if (!below_prefix(tag)) {
			problem = "the import lists a resource outside the folder of its prefab";
			return std::nullopt;
		}
		all.insert(std::string(tag.view()));
	}
	if (!all.contains(std::string(imported->prefab_tag.view())) || !below_prefix(imported->geom_tag) || !all.contains(std::string(imported->geom_tag.view()))) {
		problem = "the prefab or the geometry of the import is not among its resources";
		return std::nullopt;
	}
	std::unordered_set<std::string> created;
	for (const res::tag& tag : imported->created_tags) {
		const std::string text(tag.view());
		if (!all.contains(text) || !created.insert(text).second) {
			problem = "the import created a resource it does not list, or one twice";
			return std::nullopt;
		}
	}
	// Exactly the created resources came with the answer, each once (the bundle reader refuses a tag twice).
	if (bundle.blobs.size() != created.size()) {
		problem = "the resources of the answer are not the ones the import created";
		return std::nullopt;
	}
	for (const bundle_blob& blob : bundle.blobs) {
		if (!created.contains(blob.tag)) {
			problem = "the answer carries a resource the import did not create";
			return std::nullopt;
		}
	}
	return imported;
}

std::uint64_t edt::decoder_memory_limit(std::int64_t configured_mb, std::string& warning)
{
	warning.clear();
	if (configured_mb <= 0) return k_default_memory_mb * k_mib;
	std::uint64_t mb = static_cast<std::uint64_t>(configured_mb);
	if (mb < k_min_memory_mb) {
		warning = std::format("import/decoder_memory_mb {} is below the lowest value, {} MiB is used", configured_mb, k_min_memory_mb);
		mb = k_min_memory_mb;
	} else if (mb > k_max_memory_mb) {
		warning = std::format("import/decoder_memory_mb {} is above the highest value, {} MiB is used", configured_mb, k_max_memory_mb);
		mb = k_max_memory_mb;
	}
	return mb * k_mib;
}

edt::process_decoder::process_decoder(res::resource_system& resources, decode_runner_config config)
	: m_resources(resources)
	, m_config(std::move(config))
{
	if (m_config.decoder.empty()) {
		const fs::path self = platform::current_executable();
#if defined(_WIN32)
		m_config.decoder = self.parent_path() / "snk_model_decoder.exe";
#else
		m_config.decoder = self.parent_path() / "snk_model_decoder";
#endif
	}
}

std::string edt::process_decoder::unavailable_reason() const
{
	std::error_code ec;
	if (fs::is_regular_file(m_config.decoder, ec)) return {};
	const auto name = m_config.decoder.u8string();
	return std::format("model import for agents is not available: the model decoder '{}' was not found next to the editor (reinstall the editor)",
		std::string(name.begin(), name.end()));
}

edt::decode_outcome edt::process_decoder::run(const decode_request& request, const import_source& source, ds::task_context* task) const
{
	if (source.preloaded == nullptr) throw std::runtime_error("the decoder needs the bytes of the model");
	decode_runner_config config = m_config;
	if (source.deadline != std::chrono::steady_clock::time_point{}) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(source.deadline - std::chrono::steady_clock::now());
		config.timeout = std::clamp(left, std::chrono::milliseconds(1000), m_config.timeout);
	}
	const decode_runner runner(std::move(config));
	decode_outcome outcome = runner.run(request, *source.preloaded, task != nullptr ? task->stop : std::stop_token{});
	if (outcome.cancelled) throw ds::task_cancelled{};
	if (outcome.timed_out) throw std::runtime_error("the model took too long to decode");
	if (!outcome.ok()) {
		egLOG_WARN("edt/decoder", "{}", outcome.error);
		throw std::runtime_error(outcome.error.starts_with("the decoder failed") || outcome.error.starts_with("the decoder could not run")
			? "the decoder failed (more in the editor log)" : "the decoder's answer cannot be used (more in the editor log)");
	}
	return outcome;
}

edt::model_file_scan edt::process_decoder::scan(const import_source& source, ds::task_context* task) const
{
	decode_request request;
	request.mode = decode_request::mode_kind::scan;
	request.source_path = source.path;
	request.limits = source.limits;
	const decode_outcome outcome = run(request, source, task);
	std::string problem;
	std::optional<model_file_scan> scan = accept_scan_answer(*outcome.bundle, request, problem);
	if (!scan) {
		egLOG_WARN("edt/decoder", "the answer to a scan was refused: {}", problem);
		throw std::runtime_error("the decoder's answer was refused (more in the editor log)");
	}
	return std::move(*scan);
}

edt::import_intermediate edt::process_decoder::import(const import_source& source, ds::task_context* task) const
{
	if (source.allowed == nullptr) throw std::runtime_error("an import through the decoder needs the list of files a person was shown");
	decode_request request;
	request.mode = decode_request::mode_kind::import;
	request.source_path = source.path;
	request.allowed = *source.allowed;
	request.limits = source.limits;
	const decode_outcome outcome = run(request, source, task);

	// Nothing of the answer reaches memory:// before it passed every check.
	std::string problem;
	std::optional<import_intermediate> imported = accept_import_answer(*outcome.bundle, request, problem);
	if (!imported) {
		egLOG_WARN("edt/decoder", "the answer to an import was refused: {}", problem);
		throw std::runtime_error("the decoder's answer was refused (more in the editor log)");
	}
	if (!imported->error.empty()) return std::move(*imported);

	// An agent's import never reuses or overwrites a resource it did not store itself.
	for (const res::tag& tag : imported->created_tags) {
		if (m_resources.exists(tag)) {
			throw std::runtime_error(std::format("'{}' is already in memory: another import of a model with this name has not finished", tag.view()));
		}
	}
	std::vector<res::tag> stored;
	const auto drop_stored = [&] {
		for (const res::tag& tag : stored) m_resources.erase(tag);
	};
	try {
		for (const bundle_blob& blob : outcome.bundle->blobs) {
			const res::tag tag(blob.tag);
			if (!m_resources.store(tag, blob.bytes)) throw std::runtime_error(std::format("'{}' could not be stored", blob.tag));
			stored.push_back(tag);
		}
		if (task != nullptr) task->throw_if_stopped();
	} catch (...) {
		drop_stored();
		throw;
	}
	return std::move(*imported);
}
