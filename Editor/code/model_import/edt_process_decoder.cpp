#include "edt_process_decoder.h"
#include "edt_tag_text.h"
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_process.h>
#include <boost/json/parse_options.hpp>
#include <boost/json/stream_parser.hpp>
#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <stdexcept>
#include <stop_token>
#include <unordered_set>
#include <initializer_list>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t k_mib = 1024ull * 1024ull;
constexpr std::uint64_t k_default_memory_mb = 2048;
constexpr std::uint64_t k_min_memory_mb = 256;
constexpr std::uint64_t k_max_memory_mb = 8192;
constexpr std::size_t k_max_error_bytes = 300;

// What the child says about itself goes to an agent and into a log: cut on a code point boundary, and escaped like the
// rest of what it prints (control characters, bidi marks, separators).
std::string cleaned(std::string_view text)
{
	std::string_view cut = text.substr(0, k_max_error_bytes);
	if (cut.size() < text.size()) {
		while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.remove_suffix(1);
		if (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0xC0) cut.remove_suffix(1);
	}
	const std::string out = edt::decode_log_text(cut);
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

namespace {

// The tags a model import makes below its folder, by what they hold; anything else is refused. In particular no model
// file (.fbx, .glb, .gltf, .obj and the rest the importer's adapter reads) can come back as a resource: the exporter copies
// every resource into the project, and a model there is parsed inside the editor when something refers to it.
enum class blob_kind { desc, image, embedded_raw, embedded_image };

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

bool ends_with_text(std::string_view text, std::string_view suffix)
{
	return text.size() > suffix.size() && lower(std::string(text.substr(text.size() - suffix.size()))) == suffix;
}

std::string_view extension_of(std::string_view name)
{
	const std::size_t dot = name.rfind('.');
	return dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
}

std::optional<blob_kind> classify(std::string_view relative, std::string_view stem)
{
	if (relative == std::string(stem) + ".prefab.desc" || relative == std::string(stem) + ".geom.desc") return blob_kind::desc;
	const auto in_folder = [&relative](std::string_view folder) { return relative.starts_with(folder) && relative.size() > folder.size(); };
	if (in_folder("materials/")) {
		return ends_with_text(relative, ".mat.desc") ? std::optional(blob_kind::desc) : std::nullopt;
	}
	if (in_folder("textures/")) {
		if (ends_with_text(relative, ".txm.desc")) return blob_kind::desc;
		const std::string extension = lower(std::string(extension_of(relative)));
		if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "tga" || extension == "bmp") return blob_kind::image;
		return std::nullopt;
	}
	if (in_folder("__embedded_txm/")) {
		if (ends_with_text(relative, ".txm.desc")) return blob_kind::desc;
		const std::string extension = lower(std::string(extension_of(relative)));
		if (extension == "raw") return blob_kind::embedded_raw;
		if (extension == "pct" || extension == "png" || extension == "jpg" || extension == "jpeg") return blob_kind::embedded_image;
	}
	return std::nullopt;
}

bool starts_with_bytes(const std::vector<std::byte>& bytes, std::initializer_list<unsigned> head)
{
	if (bytes.size() < head.size()) return false;
	std::size_t i = 0;
	for (const unsigned expected : head) {
		if (std::to_integer<unsigned>(bytes[i++]) != expected) return false;
	}
	return true;
}

bool is_png(const std::vector<std::byte>& bytes) { return starts_with_bytes(bytes, { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }); }
bool is_jpeg(const std::vector<std::byte>& bytes) { return starts_with_bytes(bytes, { 0xFF, 0xD8, 0xFF }); }
bool is_bmp(const std::vector<std::byte>& bytes) { return starts_with_bytes(bytes, { 'B', 'M' }); }

// What the exporter will copy under this name must be what the name says.
bool content_matches(blob_kind kind, std::string_view relative, const std::vector<std::byte>& bytes)
{
	switch (kind) {
	case blob_kind::desc: return true;	// checked as JSON by the caller
	case blob_kind::image: {
		const std::string extension = lower(std::string(extension_of(relative)));
		if (extension == "png") return is_png(bytes);
		if (extension == "jpg" || extension == "jpeg") return is_jpeg(bytes);
		if (extension == "bmp") return is_bmp(bytes);
		return bytes.size() >= 18;	// .tga has no magic: its header is 18 bytes
	}
	case blob_kind::embedded_raw: return bytes.size() >= 8;
	case blob_kind::embedded_image: return is_png(bytes) || is_jpeg(bytes) || is_bmp(bytes);
	}
	return false;
}

// What a desc of an import may point at: the base descs and the shader the importer names, and the resources of this very
// bundle. Every other string that reads as a tag (a protocol, or a "://" in it) is refused, so a desc cannot send hot
// reload, the exporter or an agent's node.get to edt://, user://, cache://, engine://, code:// or a file of the project.
const std::unordered_set<std::string_view> k_res_references = {
	"res://base_geometry.desc", "res://base_material.desc", "res://base_texture.desc", "res://shaders/mix_opaque_trans_scene.frag" };
const std::unordered_set<std::string_view> k_parent_references = { "res://base_geometry.desc", "res://base_material.desc", "res://base_texture.desc" };
const std::unordered_set<std::string_view> k_header_keys = { "__type", "__version", "__guid", "__id", "__parent" };

// Read as res::tag reads it (edt_tag_text.h, the same functions the desc files an agent writes are checked with): "res:\\x\\y.fbx"
// is the tag res://x/y.fbx. Stricter than res::tag: any "://", and a text that only begins like a protocol ("user/x"), in
// any letter case, count too.
bool reads_as_tag(std::string_view text)
{
	const std::string normal = edt::tag_text_normalized(text);
	if (normal.find("://") != std::string::npos) return true;
	const std::string prefix = lower(std::string(edt::protocol_like_prefix(normal)));
	return prefix == "res" || edt::is_other_tag_protocol(prefix);
}

// A name that is one file on Windows or cannot be made there (a trailing dot or space, a device name, the characters
// Windows refuses), so two resources of an import cannot collide there and none is a device.
bool windows_segment_problem(std::string_view segment)
{
	if (segment.empty() || segment.back() == '.' || segment.back() == ' ') return true;
	if (segment.find_first_of("<>\"|?*") != std::string_view::npos) return true;
	const std::string stem = lower(std::string(segment.substr(0, segment.find('.'))));
	if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
	return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9';
}

bool windows_path_problem(std::string_view path)
{
	while (!path.empty()) {
		const std::size_t slash = path.find('/');
		if (windows_segment_problem(path.substr(0, slash))) return true;
		if (slash == std::string_view::npos) break;
		path.remove_prefix(slash + 1);
	}
	return false;
}

std::string json_problem(const boost::json::value& value, const std::unordered_set<std::string>& bundle_tags, std::size_t depth = 0)
{
	if (depth > 64) return "a desc of the import is nested too deep";
	if (value.is_string()) {
		const std::string_view text(value.as_string().data(), value.as_string().size());
		if (reads_as_tag(text) && !bundle_tags.contains(std::string(text)) && !k_res_references.contains(text)) {
			return "a desc of the import refers to '" + cleaned(text) + "', which is neither a resource of the import nor a base resource";
		}
		return {};
	}
	if (value.is_array()) {
		for (const boost::json::value& element : value.as_array()) {
			if (std::string why = json_problem(element, bundle_tags, depth + 1); !why.empty()) return why;
		}
		return {};
	}
	if (!value.is_object()) return {};
	for (const auto& member : value.as_object()) {
		const std::string_view key(member.key().data(), member.key().size());
		if (key.starts_with("__") && !k_header_keys.contains(key)) return "a desc of the import has the key '" + cleaned(key) + "'";
		if (reads_as_tag(key) && !bundle_tags.contains(std::string(key)) && !k_res_references.contains(key)) {
			return "a desc of the import has the key '" + cleaned(key) + "', which reads as a tag";
		}
		if (key == "__parent") {
			const boost::json::value& parent = member.value();
			const std::string_view text = parent.is_string() ? std::string_view(parent.as_string().data(), parent.as_string().size()) : std::string_view{};
			if (!k_parent_references.contains(text)) return "a desc of the import has a __parent that is not a base resource";
		}
		if (std::string why = json_problem(member.value(), bundle_tags, depth + 1); !why.empty()) return why;
	}
	return {};
}

std::string desc_blob_problem(const edt::bundle_blob& blob, const std::unordered_set<std::string>& bundle_tags)
{
	boost::json::parse_options options;
	options.max_depth = 64;
	boost::json::stream_parser parser(boost::json::storage_ptr{}, options);
	boost::system::error_code error;
	parser.write(reinterpret_cast<const char*>(blob.bytes.data()), blob.bytes.size(), error);
	if (!error) parser.finish(error);
	if (error) return "a desc of the import is not valid JSON within the limits";
	const boost::json::value value = parser.release();
	if (!value.is_object()) return "a desc of the import is not a JSON object";
	return json_problem(value, bundle_tags);
}

}

bool edt::is_import_resource_name(std::string_view relative, std::string_view stem)
{
	return classify(relative, stem).has_value();
}

std::string edt::file_extension_lowercase(std::string_view path)
{
	const std::size_t slash = path.rfind('/');
	const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
	return lower(std::string(extension_of(name)));
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

	// The folder of the import is the stem of the model, as the importer makes it; it is the editor's, not the child's.
	const std::string stem = request.source_path.stem().string();
	const std::string prefix = "memory://" + stem + "/";
	if (stem.empty() || windows_segment_problem(stem) || !is_bundle_tag(prefix + "x", bundle_limits{}.max_tag_bytes)) {
		problem = "the name of the model cannot be the folder of an import";
		return std::nullopt;
	}
	if (std::string(imported->prefab_tag.view()) != prefix + stem + ".prefab.desc" || std::string(imported->geom_tag.view()) != prefix + stem + ".geom.desc") {
		problem = "the prefab or the geometry of the import is not where the model's import puts it";
		return std::nullopt;
	}

	// An import made in an empty memory:// lists exactly what it created: a tag it only lists would be copied into the
	// project by the exporter although this import never made it.
	std::unordered_set<std::string> created;
	std::unordered_set<std::string> created_folded;
	for (const res::tag& tag : imported->created_tags) {
		// Two tags that differ only by case are one file on Windows (and on a case-insensitive disk): refused as a repeat.
		if (!created.insert(std::string(tag.view())).second || !created_folded.insert(lower(std::string(tag.view()))).second) {
			problem = "the import created a resource twice";
			return std::nullopt;
		}
	}
	std::unordered_set<std::string> listed;
	for (const res::tag& tag : imported->all_tags) listed.insert(std::string(tag.view()));
	if (listed != created) {
		problem = "the import lists resources it did not create";
		return std::nullopt;
	}
	if (!created.contains(std::string(imported->prefab_tag.view())) || !created.contains(std::string(imported->geom_tag.view()))) {
		problem = "the prefab or the geometry of the import is not among its resources";
		return std::nullopt;
	}
	// Exactly the created resources came with the answer, each once (the bundle reader refuses a tag twice).
	if (bundle.blobs.size() != created.size()) {
		problem = "the resources of the answer are not the ones the import created";
		return std::nullopt;
	}

	// What the request allows, not only what a bundle can hold: a blob is at most twice the biggest file of a model and the
	// resources together twice what a model may read; one blob cannot take the editor's memory before the exporter copies it.
	const std::uint64_t blob_limit = static_cast<std::uint64_t>(request.limits.max_file_bytes) * 2;
	const std::uint64_t total_limit = static_cast<std::uint64_t>(request.limits.max_total_bytes) * 2;
	std::uint64_t total = 0;
	for (const bundle_blob& blob : bundle.blobs) {
		if (!created.contains(blob.tag) || !blob.tag.starts_with(prefix)) {
			problem = "the answer carries a resource the import did not create";
			return std::nullopt;
		}
		const std::string_view relative = std::string_view(blob.tag).substr(prefix.size());
		if (windows_path_problem(relative)) {
			problem = "the import made a resource with a name that cannot be a file on every system: '" + cleaned(relative) + "'";
			return std::nullopt;
		}
		const std::optional<blob_kind> what = classify(relative, stem);
		if (!what) {
			problem = "the import made a resource of a kind it never makes: '" + cleaned(relative) + "'";
			return std::nullopt;
		}
		if (blob.bytes.size() > blob_limit || (total += blob.bytes.size()) > total_limit) {
			problem = "the resources of the import are bigger than the request allows";
			return std::nullopt;
		}
		if (!content_matches(*what, relative, blob.bytes)) {
			problem = "the resource '" + cleaned(relative) + "' is not what its name says";
			return std::nullopt;
		}
		if (*what == blob_kind::desc) {
			if (std::string why = desc_blob_problem(blob, created); !why.empty()) {
				problem = std::move(why);
				return std::nullopt;
			}
		}
	}
	// The prefab the editor builds from this JSON is checked like the descs of the bundle.
	if (std::string why = json_problem(imported->prefab_json, created); !why.empty()) {
		problem = std::move(why);
		return std::nullopt;
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
		config.timeout = std::min(std::max(left, std::chrono::milliseconds(1000)), m_config.timeout);
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

	// An agent's import never reuses or overwrites a resource it did not store itself. The check and the store are two calls:
	// the resolver of memory:// has no create-only store, and another writer of this folder (a person's import of a model with
	// the same name) would have to land between them; the importer in the process has the same window.
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
