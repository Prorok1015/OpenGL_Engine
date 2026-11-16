#include "edt_process_decoder.h"
#include "edt_tag_text.h"
#include <snk/common/fs_paths.h>
#include <snk/common/hash/sha256.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/text_escape.h>
#include <snk/platform/plt_process.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/resource/res_desc_binary.h>
#include <boost/json/parse_options.hpp>
#include <boost/json/stream_parser.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <initializer_list>

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t k_mib = 1024ull * 1024ull;
constexpr std::uint64_t k_default_memory_mb = 2048;
constexpr std::uint64_t k_min_memory_mb = 256;
constexpr std::uint64_t k_max_memory_mb = 8192;
constexpr std::size_t k_max_error_bytes = 300;
constexpr std::size_t k_max_scan_requests = 256;
constexpr std::size_t k_max_request_name_bytes = 1024;
constexpr int k_max_scan_rounds = 3;

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

edt::model_file_scan failed_scan(std::string error)
{
	edt::model_file_scan scan;
	scan.error = std::move(error);
	return scan;
}

std::string shown_name(const fs::path& path)
{
	return text_escape::escaped_text(fs_paths::utf8_of(path.filename()), 256);
}

std::string too_many_names(std::string_view name)
{
	return std::format("the model names '{}' in more than {} ways", text_escape::escaped_text(name, 256), edt::k_max_names_per_file);
}

const boost::json::string* string_of(const boost::json::object& object, const char* key)
{
	const boost::json::value* value = object.if_contains(key);
	return value != nullptr && value->is_string() ? &value->as_string() : nullptr;
}

// A decoder that ran without all of its sandbox: one WARN per editor run. What it says comes from the child's status line,
// written before it read the request (decoder_sandbox_status), never from its answer.
void warn_once_without_sandbox(const edt::decoder_sandbox_status& sandbox)
{
	const std::string warning = edt::decoder_sandbox_warning(sandbox);
	if (warning.empty()) return;
	static std::once_flag once;
	std::call_once(once, [&] { egLOG_WARN("edt/decoder", "{}", warning); });
}

} // namespace

std::vector<edt::provided_entry> edt::provided_entries(const std::vector<provided_file>& files, std::uintmax_t max_total_bytes,
	std::vector<bundle_blob>& blobs)
{
	std::vector<provided_entry> entries;
	entries.reserve(files.size());
	std::unordered_map<const std::vector<std::byte>*, std::uint32_t> blob_of;
	std::vector<std::size_t> names_of;
	std::uintmax_t total = 0;
	for (const provided_file& file : files) {
		provided_entry entry{ file.name, file.relative, file.kind, file.bytes != nullptr };
		if (file.bytes) {
			const auto [found, added] = blob_of.try_emplace(file.bytes.get(), static_cast<std::uint32_t>(blob_of.size()));
			if (added) {
				total += file.bytes->size();
				if (total > max_total_bytes) {
					throw std::runtime_error(std::format("the files of the model together are bigger than {} bytes", max_total_bytes));
				}
				names_of.push_back(0);
			}
			entry.blob = found->second;
			if (++names_of[entry.blob] > k_max_names_per_file) throw std::runtime_error(too_many_names(file.relative));
		}
		entries.push_back(std::move(entry));
	}
	blobs.reserve(blobs.size() + blob_of.size());
	std::vector<const std::vector<std::byte>*> in_order(blob_of.size(), nullptr);
	for (const auto& [bytes, index] : blob_of) in_order[index] = bytes;
	for (std::size_t i = 0; i < in_order.size(); ++i) blobs.push_back({ provided_blob_tag(i), *in_order[i] });
	return entries;
}

edt::model_file_scan edt::broker_scan(const import_source& source, const std::function<scan_requests(const std::vector<provided_file>&)>& run_round,
	ds::task_context* task)
{
	const bool has_deadline = source.deadline != std::chrono::steady_clock::time_point{};
	const auto past_deadline = [&] { return has_deadline && std::chrono::steady_clock::now() >= source.deadline; };
	constexpr const char* k_too_long = "the model took too long to read";

	// One access for every round: what the model may read, and how much, is counted over the whole scan.
	model_file_access access(source.path, source.limits, nullptr, source.boundary, source.on_disk);
	std::vector<provided_file> provided;
	std::set<std::pair<std::string, model_file_kind>> asked;
	// One shared_ptr per side file: each name of it carries the same bytes, so the request holds them once.
	std::map<fs::path, std::shared_ptr<const std::vector<std::byte>>> side_bytes;
	std::map<fs::path, std::size_t> side_names;
	for (int round_index = 0;; ++round_index) {
		if (past_deadline()) return failed_scan(k_too_long);
		scan_requests answer = run_round(provided);
		if (!answer.error.empty()) return failed_scan(std::move(answer.error));

		bool new_side_file = false;
		for (const requested_file& wanted : answer.requested) {
			// Asked again in a later round (the child reports every name it was not given): already answered.
			if (!asked.emplace(wanted.name, wanted.kind).second) continue;
			const resolved_reference resolved = access.resolve(wanted.name, wanted.kind);
			if (!resolved.error.empty()) {
				// The parser also asks for names of its own: Assimp's OBJ loader, missing the .mtl, falls back to one built
				// from the model's path. A side file the rule refuses is missing to the child, like one not on the disk;
				// a limit, a file that cannot be trusted, and any name of a model that is no file on the disk (#925) fail.
				const bool missing_by_rule = resolved.refusal == refusal_kind::rule && source.on_disk && wanted.kind == model_file_kind::side_file;
				if (!missing_by_rule) return failed_scan(resolved.error);
				egLOG_WARN("edt/decoder", "{}: the side-file rule refuses it, so the model is read without it", resolved.error);
				new_side_file = true;
				provided.push_back({ wanted.name, {}, model_file_kind::side_file, nullptr });
				continue;
			}
			if (past_deadline()) return failed_scan(k_too_long);
			try {
				// A texture changes nothing in the parse: the editor only reads and hashes it for the import.
				if (wanted.kind == model_file_kind::texture) {
					access.hash_listed(resolved);
					continue;
				}
				new_side_file = true;
				provided_file file{ wanted.name, {}, model_file_kind::side_file, nullptr };
				if (resolved.found()) {
					file.relative = access.relative_name(resolved);
					if (++side_names[resolved.path] > k_max_names_per_file) return failed_scan(too_many_names(file.relative));
					std::shared_ptr<const std::vector<std::byte>>& bytes = side_bytes[resolved.path];
					if (!bytes) {
						std::optional<std::vector<std::byte>> read = access.read(resolved);
						if (!read) return failed_scan(std::format("'{}' cannot be read", shown_name(resolved.path)));
						bytes = std::make_shared<const std::vector<std::byte>>(std::move(*read));
					}
					file.bytes = bytes;
				}
				provided.push_back(std::move(file));
			} catch (const std::runtime_error& e) {
				// Not an image, changed while it was read, over the budget.
				return failed_scan(e.what());
			}
		}
		if (!new_side_file) break;
		if (round_index + 1 == k_max_scan_rounds) return failed_scan(std::format("the model asks for its files in more than {} rounds", k_max_scan_rounds));
		if (task != nullptr) task->throw_if_stopped();
	}
	model_file_scan out;
	out.files = access.files();
	out.total_bytes = access.listed_bytes();
	return out;
}

std::optional<edt::scan_requests> edt::accept_scan_answer(const decode_bundle& bundle, std::string& problem)
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
		scan_requests failed;
		failed.error = cleaned(*error);
		if (failed.error.empty()) failed.error = "the model could not be read";
		return failed;
	}
	const boost::json::value* list = manifest.if_contains("requests");
	if (list == nullptr) {
		problem = "the answer to a scan holds no requests";
		return std::nullopt;
	}
	if (list->is_array() && list->as_array().size() > k_max_scan_requests) {
		problem = "the scan asks for more files than it may";
		return std::nullopt;
	}
	std::optional<scan_requests> scan = scan_requests_from_json(manifest);
	if (!scan || !scan->error.empty()) {
		problem = "the answer to a scan cannot be read";
		return std::nullopt;
	}
	for (const requested_file& file : scan->requested) {
		const bool readable = !file.name.empty() && file.name.size() <= k_max_request_name_bytes
			&& std::none_of(file.name.begin(), file.name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		if (!readable) {
			problem = "the scan asks for a file by a name that cannot be one";
			return std::nullopt;
		}
	}
	return scan;
}

namespace {

// The tags a model import makes below its folder, by what they hold; anything else is refused. In particular no model
// file (.fbx, .glb, .gltf, .obj and the rest the importer's adapter reads) can come back as a resource: the exporter copies
// every resource into the project, and a model there is parsed inside the editor when something refers to it.
// geom_bin: the binary file of the geometry (EPIC-79, #1068 PR3b), decoded and checked as the loader reads it.
enum class blob_kind { desc, image, embedded_raw, embedded_image, geom_bin };

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
	if (relative == std::string(stem) + ".geom.bin") return blob_kind::geom_bin;
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
	case blob_kind::geom_bin: return true;	// decoded with its desc by geometry_problem
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
// No __guid: the ID of an imported file is the .resource the editor gives it (EPIC-57, #772).
const std::unordered_set<std::string_view> k_header_keys = { "__type", "__version", "__id", "__parent" };

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
		if (key == "__guid") return "a desc of the import has the key '__guid': the editor gives each imported file its ID (.resource)";
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

const edt::bundle_blob* blob_named(const edt::decode_bundle& bundle, std::string_view tag)
{
	for (const edt::bundle_blob& blob : bundle.blobs) {
		if (blob.tag == tag) return &blob;
	}
	return nullptr;
}

// The geometry of an import (#1068 PR3b): its desc and, for triangles, its binary file are decoded here exactly as the
// loader decodes them (desc_binary::parse: header, layouts, ranges inside the file and apart; every SHA-256 computed here;
// then the loader's data checks). Triangles always come as the pair, lines and points (and a model without meshes) as
// arrays. With `rewritten`, a pair is encoded again by the editor's own writer from the decoded data: no offset, hash or
// header field of the child's is stored.
std::string geometry_problem(const edt::decode_bundle& bundle, const res::tag& geom_tag, std::string_view binary_tag,
	rnd::geometry_files* rewritten)
{
	const edt::bundle_blob* desc_blob = blob_named(bundle, geom_tag.view());
	if (desc_blob == nullptr) return "the geometry desc did not come with the answer";
	const edt::bundle_blob* binary_blob = blob_named(bundle, binary_tag);

	boost::json::parse_options options;
	options.max_depth = 64;
	boost::json::stream_parser parser(boost::json::storage_ptr{}, options);
	boost::system::error_code error;
	parser.write(reinterpret_cast<const char*>(desc_blob->bytes.data()), desc_blob->bytes.size(), error);
	if (!error) parser.finish(error);
	if (error) return "the geometry desc is not valid JSON within the limits";
	const boost::json::value value = parser.release();
	if (!value.is_object()) return "the geometry desc is not a JSON object";
	const boost::json::object& desc = value.as_object();

	// Ranges are objects ({offset, size, sha256, layout}); a desc without "vertices" holds no data (a model without meshes).
	const auto is_range = [&desc](std::string_view key) {
		const boost::json::value* found = desc.if_contains(key);
		return found != nullptr && found->is_object();
	};
	const bool ranges = is_range("vertices") || is_range("indices");
	const rnd::geometry_topology topology = rnd::topology_of(desc);
	if (!ranges) {
		if (binary_blob != nullptr) return "a binary file came with a geometry that does not refer to one";
		const boost::json::value* vertices = desc.if_contains("vertices");
		const bool holds_vertices = vertices != nullptr && vertices->is_array() && !vertices->as_array().empty();
		if (topology == rnd::geometry_topology::triangles && holds_vertices) {
			return "the triangles of the geometry came as arrays, not as its binary file";
		}
	} else {
		if (binary_blob == nullptr) return "the geometry refers to a binary file that did not come with the answer";
		if (topology != rnd::geometry_topology::triangles) return "a geometry of lines or points came with a binary file";
	}

	rnd::geometry_desc data;
	const std::span<const std::byte> binary = binary_blob != nullptr ? std::span<const std::byte>(binary_blob->bytes) : std::span<const std::byte>{};
	if (std::string why = rnd::decode_geometry_files(geom_tag, desc, binary, data); !why.empty()) return why;

	if (binary_blob != nullptr && rewritten != nullptr) {
		rnd::geometry_file_options encode;
		// desc_blob_problem allowed only a base resource as the parent; nothing else of the child's header is kept.
		if (const boost::json::value* parent = desc.if_contains("__parent"); parent != nullptr && parent->is_string()) {
			encode.header["__parent"] = *parent;
		}
		encode.bounds = desc.contains("bounds");
		*rewritten = rnd::encode_geometry_files(geom_tag, data, encode);
		if (!*rewritten) return rewritten->error;
	}
	return {};
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

std::optional<edt::import_intermediate> edt::accept_import_answer(const decode_bundle& bundle, const decode_request& request, std::string& problem,
	rnd::geometry_files* geometry)
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

	// The folder of the import is the stem of the model (or the folder the request named), as the importer makes it; it is
	// the editor's, not the child's.
	const std::string stem = request.source_path.stem().string();
	const std::string folder = request.memory_folder.empty() ? stem + "/" : request.memory_folder;
	const std::string prefix = "memory://" + folder;
	if (stem.empty() || windows_segment_problem(stem) || !folder.ends_with('/') || windows_path_problem(std::string_view(folder).substr(0, folder.size() - 1))
		|| !is_bundle_tag(prefix + "x", bundle_limits{}.max_tag_bytes)) {
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
	// The binary file of the geometry is created but not listed (whoever copies the desc carries it along).
	const std::string binary_tag = prefix + stem + ".geom.bin";
	std::unordered_set<std::string> listed;
	for (const res::tag& tag : imported->all_tags) listed.insert(std::string(tag.view()));
	if (listed.contains(binary_tag)) {
		problem = "the import lists the binary file of its geometry";
		return std::nullopt;
	}
	if (created.contains(binary_tag)) listed.insert(binary_tag);
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
		// textures/* images are files the model named; a model that is no file on the disk reads none (embedded images
		// are __embedded_txm/*).
		if (!request.on_disk && *what == blob_kind::image) {
			problem = "the import of a model that is not a file on the disk carries the image file '" + cleaned(relative) + "'";
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
				problem = "'" + cleaned(relative) + "': " + why;
				return std::nullopt;
			}
		}
	}
	if (std::string why = geometry_problem(bundle, imported->geom_tag, binary_tag, geometry); !why.empty()) {
		problem = "the geometry of the import is refused: " + cleaned(why);
		return std::nullopt;
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

edt::sandbox_mode edt::decoder_sandbox_mode(std::string_view configured, std::string& warning)
{
	warning.clear();
	if (configured == "on") return sandbox_mode::best_effort;
	if (const std::optional<sandbox_mode> mode = sandbox_mode_from_name(configured)) return *mode;
	warning = std::format("import/decoder_sandbox '{}' is none of on, off, required: on is used", text_escape::escaped_text(configured, 64));
	return sandbox_mode::best_effort;
}

edt::decoder_steps edt::steps_of(std::shared_ptr<const process_decoder> decoder)
{
	decoder_steps steps;
	steps.scan = [decoder](const import_source& source, ds::task_context* task) { return decoder->scan(source, task); };
	steps.import = [decoder](const import_source& source, ds::task_context* task) { return decoder->import(source, task); };
	steps.unavailable_reason = [decoder] { return decoder->unavailable_reason(); };
	steps.version = [decoder] { return decoder->version(); };
	return steps;
}

std::optional<res::tag> edt::erase_memory_folder(res::resource_system& resources, const std::string& folder,
	const std::vector<res::tag>* known)
{
	std::optional<std::vector<res::tag>> listed = resources.files_under(res::tag::memory, folder);
	if (!listed) {
		egLOG_WARN("edt/decoder", "memory:// cannot list '{}': only what this run made there is erased", folder);
		listed.emplace(known ? *known : std::vector<res::tag>{});
	}
	const std::vector<res::tag>& found = *listed;
	for (const res::tag& tag : found) resources.erase(tag);
	for (const res::tag& tag : found) {
		if (resources.exists(tag)) return tag;
	}
	return std::nullopt;
}

edt::process_decoder::process_decoder(res::resource_system& resources, decode_runner_config config, sandbox_listener on_sandbox)
	: m_resources(resources)
	, m_config(std::move(config))
	, m_on_sandbox(std::move(on_sandbox))
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
	return std::format("the model decoder '{}' was not found next to the editor (reinstall the editor)",
		text_escape::escaped_text(fs_paths::generic_utf8_of(m_config.decoder), 1024));
}

namespace {

std::mutex g_file_versions_mutex;
std::unordered_map<std::string, std::string> g_file_versions;
std::atomic<std::size_t> g_file_version_hashes{ 0 };

}

std::optional<std::string> edt::file_version(const std::filesystem::path& file)
{
	std::error_code size_error;
	std::error_code time_error;
	const std::uintmax_t size = fs::file_size(file, size_error);
	const auto written = fs::last_write_time(file, time_error);
	if (size_error || time_error) return std::nullopt;
	const std::string key = std::format("{}|{}|{}", fs_paths::generic_utf8_of(file), size,
		written.time_since_epoch().count());

	// Held while hashing: two decoders made at once hash the file once.
	const std::lock_guard guard(g_file_versions_mutex);
	if (const auto found = g_file_versions.find(key); found != g_file_versions.end()) return found->second;
	++g_file_version_hashes;
	std::optional<std::string> hash = cmn::sha256_file_hex(file);
	if (hash) g_file_versions.emplace(key, *hash);
	return hash;
}

std::size_t edt::file_version_hashes()
{
	return g_file_version_hashes.load();
}

std::string edt::process_decoder::version() const
{
	std::call_once(m_version_once, [this] {
		if (std::optional<std::string> hash = edt::file_version(m_config.decoder)) {
			m_version = std::move(*hash);
			return;
		}
		egLOG_WARN("edt/decoder", "the model decoder '{}' cannot be read: every model load runs it, none is reused",
			text_escape::escaped_text(fs_paths::generic_utf8_of(m_config.decoder), 1024));
	});
	return m_version;
}

edt::decode_outcome edt::process_decoder::run(const decode_request& request, const std::vector<bundle_blob>& files, const import_source& source,
	ds::task_context* task) const
{
	if (source.preloaded == nullptr) throw std::runtime_error("the decoder needs the bytes of the model");
	decode_runner_config config = m_config;
	if (source.deadline != std::chrono::steady_clock::time_point{}) {
		// The scan's rounds and the editor's reads share the one deadline: no round gets time of its own.
		const auto left = std::chrono::ceil<std::chrono::milliseconds>(source.deadline - std::chrono::steady_clock::now());
		if (left <= std::chrono::milliseconds::zero()) throw std::runtime_error("the model took too long to decode");
		config.timeout = std::min(left, m_config.timeout);
	}
	config.sandbox = effective_sandbox_mode(m_config.sandbox, source.sandbox);
	const sandbox_mode mode = config.sandbox;
	const decode_runner runner(std::move(config));
	decode_outcome outcome = runner.run(request, *source.preloaded, files, task != nullptr ? task->stop : std::stop_token{});
	// Before any check of the answer (#1076): a child that crashed, timed out or answered garbage may have parsed the model
	// without its sandbox all the same. One that ran without saying how it is sandboxed counts as having none; one that
	// refused to run, or was cancelled before it said anything, parsed nothing.
	if (outcome.started && !outcome.cancelled && !outcome.sandbox_refused && !outcome.sandbox.reported) {
		outcome.sandbox = unreported_decoder_sandbox();
	}
	if (outcome.sandbox.reported) {
		warn_once_without_sandbox(outcome.sandbox);
		if (m_on_sandbox) m_on_sandbox(outcome.sandbox);
	}
	if (outcome.cancelled) throw ds::task_cancelled{};
	if (outcome.timed_out) throw std::runtime_error("the model took too long to decode");
	if (outcome.sandbox_refused) {
		// Outside `required` the child refuses only when a sandbox the system has failed to set up: a defect, so the person
		// is told how to go on without it.
		if (mode != sandbox_mode::required) outcome.error += "; the sandbox can be turned off in the setting import/decoder_sandbox";
		egLOG_WARN("edt/decoder", "{}", outcome.error);
		throw std::runtime_error(outcome.error);
	}
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
	request.boundary = source.boundary;
	request.memory_folder = source.memory_folder;
	request.on_disk = source.on_disk;

	decoder_sandbox_status sandbox;
	model_file_scan scanned = broker_scan(source, [&](const std::vector<provided_file>& provided) {
		std::vector<bundle_blob> blobs;
		request.provided = provided_entries(provided, source.limits.max_total_bytes, blobs);
		const decode_outcome outcome = run(request, blobs, source, task);
		sandbox = outcome.sandbox;
		std::string problem;
		std::optional<scan_requests> answer = accept_scan_answer(*outcome.bundle, problem);
		if (!answer) {
			egLOG_WARN("edt/decoder", "the answer to a scan was refused: {}", problem);
			throw std::runtime_error("the decoder's answer was refused (more in the editor log)");
		}
		return std::move(*answer);
	}, task);
	scanned.sandbox = std::move(sandbox);
	return scanned;
}

edt::import_intermediate edt::process_decoder::import(const import_source& source, ds::task_context* task) const
{
	if (source.allowed == nullptr) throw std::runtime_error("an import through the decoder needs the list of files a person was shown");
	decode_request request;
	request.mode = decode_request::mode_kind::import;
	request.source_path = source.path;
	request.limits = source.limits;
	request.boundary = source.boundary;
	request.memory_folder = source.memory_folder;
	request.on_disk = source.on_disk;

	// Exactly the listed files, read again and checked against what the person was shown, under every name the model
	// used for them in the scan.
	model_file_access access(source.path, source.limits, source.allowed, source.boundary, source.on_disk);
	std::vector<provided_file> provided;
	for (const referenced_file& file : *source.allowed) {
		const std::string name = shown_name(file.path);
		if (file.names.empty()) throw std::runtime_error(std::format("'{}' was listed without the names the model uses for it", name));
		if (file.names.size() > k_max_names_per_file) throw std::runtime_error(too_many_names(fs_paths::utf8_of(file.path.filename())));
		std::shared_ptr<const std::vector<std::byte>> bytes;
		for (const std::string& reference : file.names) {
			const resolved_reference resolved = access.resolve(reference, file.kind);
			if (!resolved.error.empty()) throw std::runtime_error(resolved.error);
			if (!resolved.found() || resolved.path != file.path) throw std::runtime_error(std::format("'{}' is gone since it was listed", name));
			if (!bytes) {
				std::optional<std::vector<std::byte>> content = access.read(resolved);
				if (!content) throw std::runtime_error(std::format("'{}' cannot be read", name));
				bytes = std::make_shared<const std::vector<std::byte>>(std::move(*content));
			}
			provided.push_back({ reference, access.relative_name(resolved), file.kind, bytes });
		}
	}
	std::vector<bundle_blob> blobs;
	request.provided = provided_entries(provided, source.limits.max_total_bytes, blobs);
	if (task != nullptr) task->throw_if_stopped();
	const decode_outcome outcome = run(request, blobs, source, task);

	// Nothing of the answer reaches memory:// before it passed every check.
	std::string problem;
	rnd::geometry_files geometry;
	std::optional<import_intermediate> imported = accept_import_answer(*outcome.bundle, request, problem, &geometry);
	if (!imported) {
		egLOG_WARN("edt/decoder", "the answer to an import was refused: {}", problem);
		throw std::runtime_error("the decoder's answer was refused (more in the editor log)");
	}
	imported->sandbox = outcome.sandbox;
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
	const auto store = [&](const res::tag& tag, const std::vector<std::byte>& bytes) {
		if (!m_resources.store(tag, bytes)) throw std::runtime_error(std::format("'{}' could not be stored", tag.view()));
		stored.push_back(tag);
		imported->created_digests.emplace_back(tag, digest_of(bytes));
	};
	// A geometry pair is the editor's own encoding of the checked data (geometry_problem), stored as the writer stores it:
	// the binary file first, then the desc, under the save lock of the desc.
	const bool pair = !geometry.binary.empty();
	const std::optional<res::tag> binary_tag = pair ? res::desc_binary::binary_tag(imported->geom_tag) : std::nullopt;
	try {
		for (const bundle_blob& blob : outcome.bundle->blobs) {
			const res::tag tag(blob.tag);
			if (pair && (tag == imported->geom_tag || tag == *binary_tag)) continue;
			store(tag, blob.bytes);
		}
		if (pair) {
			const res::desc_binary::save_lock lock(imported->geom_tag);
			store(*binary_tag, geometry.binary);
			store(imported->geom_tag, geometry.desc);
		}
		if (task != nullptr) task->throw_if_stopped();
	} catch (...) {
		drop_stored();
		throw;
	}
	return std::move(*imported);
}
