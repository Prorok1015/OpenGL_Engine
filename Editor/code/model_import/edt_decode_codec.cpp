#include "edt_decode_codec.h"
#include "edt_decode_bundle.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace json = boost::json;

namespace {

constexpr std::size_t k_max_items = 1u << 20;
constexpr std::size_t k_max_name_bytes = 1024;

std::optional<std::uint64_t> u64_from(const json::value* value)
{
	if (!value || !value->is_string()) return std::nullopt;
	const std::string_view text = value->as_string();
	std::uint64_t out = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
	if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
	return out;
}

json::value u64_to(std::uint64_t value)
{
	return json::value(std::to_string(value));
}

std::optional<std::string> string_from(const json::value* value)
{
	if (!value || !value->is_string()) return std::nullopt;
	return std::string(value->as_string());
}

std::optional<std::vector<std::string>> strings_from(const json::value* value)
{
	if (!value || !value->is_array() || value->as_array().size() > k_max_items) return std::nullopt;
	std::vector<std::string> out;
	out.reserve(value->as_array().size());
	for (const json::value& item : value->as_array()) {
		if (!item.is_string()) return std::nullopt;
		out.emplace_back(item.as_string());
	}
	return out;
}

json::array strings_to(const std::vector<std::string>& texts)
{
	json::array out;
	for (const std::string& text : texts) out.emplace_back(text);
	return out;
}

std::vector<std::string> tag_texts(const std::vector<res::tag>& tags)
{
	std::vector<std::string> out;
	out.reserve(tags.size());
	for (const res::tag& tag : tags) out.emplace_back(tag.string());
	return out;
}

std::string path_text(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return std::string(text.begin(), text.end());
}

std::filesystem::path path_from(const std::string& text)
{
	return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

const char* kind_text(edt::model_file_kind kind)
{
	return kind == edt::model_file_kind::side_file ? "side_file" : "texture";
}

std::optional<edt::model_file_kind> kind_from(const json::value* value)
{
	const auto text = string_from(value);
	if (!text) return std::nullopt;
	if (*text == "side_file") return edt::model_file_kind::side_file;
	if (*text == "texture") return edt::model_file_kind::texture;
	return std::nullopt;
}

bool fits_name(const std::string& text, std::size_t min_bytes)
{
	return text.size() >= min_bytes && text.size() <= k_max_name_bytes && text.find('\0') == std::string::npos;
}

}

std::string edt::digest_to_hex(const content_digest& digest)
{
	constexpr char k_digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(digest.size() * 2);
	for (const std::uint8_t byte : digest) {
		out.push_back(k_digits[byte >> 4]);
		out.push_back(k_digits[byte & 0x0F]);
	}
	return out;
}

std::optional<edt::content_digest> edt::digest_from_hex(const std::string& text)
{
	content_digest out{};
	if (text.size() != out.size() * 2) return std::nullopt;
	for (std::size_t i = 0; i < out.size(); ++i) {
		std::uint8_t byte = 0;
		const auto [end, error] = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, byte, 16);
		if (error != std::errc{} || end != text.data() + i * 2 + 2) return std::nullopt;
		out[i] = byte;
	}
	return out;
}

json::object edt::to_json(const referenced_file& file)
{
	json::object out;
	out["path"] = path_text(file.path);
	out["size"] = u64_to(file.size);
	out["modified"] = std::to_string(file.modified.time_since_epoch().count());
	out["volume"] = u64_to(file.id.volume);
	out["index"] = u64_to(file.id.index);
	out["index_high"] = u64_to(file.id.index_high);
	out["id_valid"] = file.id.valid;
	out["hashed"] = file.hashed;
	out["digest"] = digest_to_hex(file.digest);
	return out;
}

std::optional<edt::referenced_file> edt::referenced_file_from_json(const json::value& value)
{
	if (!value.is_object()) return std::nullopt;
	const json::object& object = value.as_object();
	const auto path = string_from(object.if_contains("path"));
	const auto size = u64_from(object.if_contains("size"));
	const auto volume = u64_from(object.if_contains("volume"));
	const auto index = u64_from(object.if_contains("index"));
	const auto index_high = u64_from(object.if_contains("index_high"));
	const auto digest_text = string_from(object.if_contains("digest"));
	const json::value* id_valid = object.if_contains("id_valid");
	const json::value* hashed = object.if_contains("hashed");
	const auto modified_text = string_from(object.if_contains("modified"));
	if (!path || !size || !volume || !index || !index_high || !digest_text || !modified_text) return std::nullopt;
	if (!id_valid || !id_valid->is_bool() || !hashed || !hashed->is_bool()) return std::nullopt;
	const auto digest = digest_from_hex(*digest_text);
	if (!digest) return std::nullopt;
	std::int64_t ticks = 0;
	const auto [end, error] = std::from_chars(modified_text->data(), modified_text->data() + modified_text->size(), ticks);
	if (error != std::errc{} || end != modified_text->data() + modified_text->size()) return std::nullopt;

	referenced_file file;
	file.path = path_from(*path);
	file.size = *size;
	file.modified = std::filesystem::file_time_type(std::filesystem::file_time_type::duration(ticks));
	file.id.volume = *volume;
	file.id.index = *index;
	file.id.index_high = *index_high;
	file.id.valid = id_valid->as_bool();
	file.digest = *digest;
	file.hashed = hashed->as_bool();
	return file;
}

json::object edt::to_json(const model_file_scan& scan)
{
	json::array files;
	for (const referenced_file& file : scan.files) files.emplace_back(to_json(file));
	json::object out;
	out["files"] = std::move(files);
	out["total_bytes"] = u64_to(scan.total_bytes);
	out["error"] = scan.error;
	return out;
}

std::optional<edt::model_file_scan> edt::model_file_scan_from_json(const json::value& value)
{
	if (!value.is_object()) return std::nullopt;
	const json::object& object = value.as_object();
	const json::value* files = object.if_contains("files");
	const auto total = u64_from(object.if_contains("total_bytes"));
	const auto error = string_from(object.if_contains("error"));
	if (!files || !files->is_array() || files->as_array().size() > k_max_items || !total || !error) return std::nullopt;

	model_file_scan scan;
	scan.total_bytes = *total;
	scan.error = *error;
	for (const json::value& item : files->as_array()) {
		auto file = referenced_file_from_json(item);
		if (!file) return std::nullopt;
		scan.files.push_back(std::move(*file));
	}
	return scan;
}

json::object edt::to_json(const scan_requests& requests)
{
	json::object out;
	if (!requests.error.empty()) {
		out["error"] = requests.error;
		return out;
	}
	json::array list;
	for (const requested_file& file : requests.requested) {
		json::object entry;
		entry["name"] = file.name;
		entry["kind"] = kind_text(file.kind);
		list.emplace_back(std::move(entry));
	}
	out["requests"] = std::move(list);
	return out;
}

std::optional<edt::scan_requests> edt::scan_requests_from_json(const json::value& value)
{
	if (!value.is_object()) return std::nullopt;
	const json::object& object = value.as_object();
	scan_requests out;
	if (const json::value* error = object.if_contains("error")) {
		const auto text = string_from(error);
		if (!text) return std::nullopt;
		out.error = *text;
		return out;
	}
	const json::value* list = object.if_contains("requests");
	if (!list || !list->is_array() || list->as_array().size() > k_max_items) return std::nullopt;
	out.requested.reserve(list->as_array().size());
	for (const json::value& item : list->as_array()) {
		if (!item.is_object()) return std::nullopt;
		const auto name = string_from(item.as_object().if_contains("name"));
		const auto kind = kind_from(item.as_object().if_contains("kind"));
		if (!name || !kind) return std::nullopt;
		out.requested.push_back({ *name, *kind });
	}
	return out;
}

json::object edt::to_json(const import_intermediate& imported)
{
	json::array weights;
	for (const std::vector<std::uint32_t>& column : imported.skin_weights) {
		json::array row;
		row.reserve(column.size());
		for (const std::uint32_t weight : column) row.emplace_back(static_cast<std::int64_t>(weight));
		weights.emplace_back(std::move(row));
	}
	json::object out;
	out["geom_tag"] = std::string(imported.geom_tag.string());
	out["prefab_tag"] = std::string(imported.prefab_tag.string());
	out["all_tags"] = strings_to(tag_texts(imported.all_tags));
	out["created_tags"] = strings_to(tag_texts(imported.created_tags));
	out["source_path"] = path_text(imported.source_path);
	out["prefab_json"] = imported.prefab_json;
	out["skin_weights"] = std::move(weights);
	out["error"] = imported.error;
	return out;
}

std::optional<edt::import_intermediate> edt::import_intermediate_from_json(const json::value& value)
{
	if (!value.is_object()) return std::nullopt;
	const json::object& object = value.as_object();
	const auto geom = string_from(object.if_contains("geom_tag"));
	const auto prefab = string_from(object.if_contains("prefab_tag"));
	const auto all_tags = strings_from(object.if_contains("all_tags"));
	const auto created_tags = strings_from(object.if_contains("created_tags"));
	const auto source = string_from(object.if_contains("source_path"));
	const auto error = string_from(object.if_contains("error"));
	const json::value* prefab_json = object.if_contains("prefab_json");
	const json::value* weights = object.if_contains("skin_weights");
	if (!geom || !prefab || !all_tags || !created_tags || !source || !error) return std::nullopt;
	if (!prefab_json || !prefab_json->is_object() || !weights || !weights->is_array() || weights->as_array().size() > k_max_items) return std::nullopt;

	// The tags name resources the editor will store: nothing but memory://, whatever the other process says.
	constexpr std::size_t k_tag_limit = 512;
	const auto acceptable = [](const std::string& tag) { return tag.empty() || is_bundle_tag(tag, k_tag_limit); };
	if (!acceptable(*geom) || !acceptable(*prefab)) return std::nullopt;
	if (!std::ranges::all_of(*all_tags, acceptable) || !std::ranges::all_of(*created_tags, acceptable)) return std::nullopt;

	import_intermediate out;
	out.geom_tag = res::tag(*geom);
	out.prefab_tag = res::tag(*prefab);
	for (const std::string& text : *all_tags) out.all_tags.emplace_back(text);
	for (const std::string& text : *created_tags) out.created_tags.emplace_back(text);
	out.source_path = path_from(*source);
	out.prefab_json = prefab_json->as_object();
	out.error = *error;
	for (const json::value& row : weights->as_array()) {
		if (!row.is_array() || row.as_array().size() > k_max_items) return std::nullopt;
		std::vector<std::uint32_t> column;
		column.reserve(row.as_array().size());
		for (const json::value& weight : row.as_array()) {
			if (!weight.is_int64() && !weight.is_uint64()) return std::nullopt;
			const std::uint64_t raw = weight.is_int64() ? static_cast<std::uint64_t>(std::max<std::int64_t>(weight.as_int64(), -1)) : weight.as_uint64();
			if (weight.is_int64() && weight.as_int64() < 0) return std::nullopt;
			if (raw > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
			column.push_back(static_cast<std::uint32_t>(raw));
		}
		out.skin_weights.push_back(std::move(column));
	}
	return out;
}

boost::json::object edt::to_json(const decode_request& request)
{
	json::object out;
	out["mode"] = request.mode == decode_request::mode_kind::scan ? "scan" : "import";
	out["source_path"] = path_text(request.source_path);
	json::array provided;
	for (const provided_entry& file : request.provided) {
		json::object entry;
		entry["name"] = file.name;
		entry["relative"] = file.relative;
		entry["kind"] = kind_text(file.kind);
		entry["present"] = file.present;
		provided.emplace_back(std::move(entry));
	}
	out["provided"] = std::move(provided);
	json::object limits;
	limits["max_total_bytes"] = u64_to(request.limits.max_total_bytes);
	limits["max_file_bytes"] = u64_to(request.limits.max_file_bytes);
	limits["max_files"] = u64_to(request.limits.max_files);
	out["limits"] = std::move(limits);
	out["deadline_ms"] = u64_to(request.deadline_ms);
	out["memory_limit_bytes"] = u64_to(request.memory_limit_bytes);
	out["boundary"] = path_text(request.boundary);
	out["memory_folder"] = request.memory_folder;
	out["on_disk"] = request.on_disk;
	return out;
}

std::optional<edt::decode_request> edt::decode_request_from_json(const boost::json::value& value)
{
	if (!value.is_object()) return std::nullopt;
	const json::object& object = value.as_object();
	decode_request request;

	const auto mode = string_from(object.if_contains("mode"));
	if (!mode) return std::nullopt;
	if (*mode == "scan") request.mode = decode_request::mode_kind::scan;
	else if (*mode == "import") request.mode = decode_request::mode_kind::import;
	else return std::nullopt;

	const auto source = string_from(object.if_contains("source_path"));
	if (!source || source->empty()) return std::nullopt;
	request.source_path = path_from(*source);

	const json::value* provided = object.if_contains("provided");
	if (!provided || !provided->is_array() || provided->as_array().size() > k_max_items) return std::nullopt;
	for (const json::value& item : provided->as_array()) {
		if (!item.is_object()) return std::nullopt;
		const json::object& entry = item.as_object();
		const auto name = string_from(entry.if_contains("name"));
		const auto relative = string_from(entry.if_contains("relative"));
		const auto kind = kind_from(entry.if_contains("kind"));
		const json::value* present = entry.if_contains("present");
		if (!name || !relative || !kind || !present || !present->is_bool()) return std::nullopt;
		// A file that is there has its name below the boundary; one that is not has none.
		if (!fits_name(*name, 1) || !fits_name(*relative, present->as_bool() ? 1 : 0)) return std::nullopt;
		if (!present->as_bool() && !relative->empty()) return std::nullopt;
		request.provided.push_back({ *name, *relative, *kind, present->as_bool() });
	}

	const json::value* limits = object.if_contains("limits");
	if (!limits || !limits->is_object()) return std::nullopt;
	const auto total = u64_from(limits->as_object().if_contains("max_total_bytes"));
	const auto per_file = u64_from(limits->as_object().if_contains("max_file_bytes"));
	const auto files = u64_from(limits->as_object().if_contains("max_files"));
	if (!total || !per_file || !files || *files > std::numeric_limits<std::size_t>::max()) return std::nullopt;
	request.limits.max_total_bytes = *total;
	request.limits.max_file_bytes = *per_file;
	request.limits.max_files = static_cast<std::size_t>(*files);

	const auto deadline = u64_from(object.if_contains("deadline_ms"));
	const auto memory = u64_from(object.if_contains("memory_limit_bytes"));
	if (!deadline || !memory) return std::nullopt;
	request.deadline_ms = *deadline;
	request.memory_limit_bytes = *memory;

	const auto boundary = string_from(object.if_contains("boundary"));
	const auto memory_folder = string_from(object.if_contains("memory_folder"));
	const json::value* on_disk = object.if_contains("on_disk");
	if (!boundary || !memory_folder || !on_disk || !on_disk->is_bool()) return std::nullopt;
	request.boundary = path_from(*boundary);
	request.memory_folder = *memory_folder;
	request.on_disk = on_disk->as_bool();
	return request;
}
