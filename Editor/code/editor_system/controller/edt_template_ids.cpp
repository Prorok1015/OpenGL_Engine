#include "edt_template_ids.h"
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/value.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_pretty_json.h>
#include <snk/resource/res_reference.h>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::uintmax_t k_sidecar_max_bytes = 64u * 1024u;
	constexpr std::uintmax_t k_desc_max_bytes = 4u * 1024u * 1024u;

	using id_map = std::unordered_map<res::asset_id, res::asset_id>;

	// Not path::string(): on Windows it goes through the ANSI code page and throws for what that cannot hold.
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.generic_u8string();
		return std::string(text.begin(), text.end());
	}

	bool is_hidden(const fs::path& path)
	{
		const fs::path name = path.filename();
		return !name.empty() && name.native().front() == '.';
	}

	bool is_desc_name(const fs::path& path)
	{
		const std::u8string name = path.filename().u8string();
		constexpr std::u8string_view suffix = u8".desc";
		if (name.size() < suffix.size()) {
			return false;
		}
		for (std::size_t i = 0; i < suffix.size(); ++i) {
			char8_t c = name[name.size() - suffix.size() + i];
			if (c >= u8'A' && c <= u8'Z') {
				c = static_cast<char8_t>(c - u8'A' + u8'a');
			}
			if (c != suffix[i]) {
				return false;
			}
		}
		return true;
	}

	bool is_sidecar(const fs::path& path)
	{
		const std::u8string name = path.filename().u8string();
		return res::is_asset_meta_name(std::string_view(reinterpret_cast<const char*>(name.data()), name.size()));
	}

	std::optional<std::string> read_capped(const fs::path& file, std::uintmax_t max_bytes)
	{
		std::error_code ec;
		const std::uintmax_t size = fs::file_size(file, ec);
		if (ec || size > max_bytes) {
			return std::nullopt;
		}
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			return std::nullopt;
		}
		std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		if (in.bad()) {
			return std::nullopt;
		}
		return text;
	}

	bool write_text(const fs::path& file, std::string_view text)
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		out.close();
		return !out.fail();
	}

	std::string pretty(const json::value& value)
	{
		std::ostringstream os;
		res::write_pretty_json(os, value);
		return os.str();
	}

	// A root folder named by the manifest, kept only when it is a plain relative path inside the project.
	bool is_plain_relative(const fs::path& root)
	{
		if (root.empty() || root.is_absolute() || root.has_root_name()) {
			return false;
		}
		for (const fs::path& part : root) {
			if (part == "..") {
				return false;
			}
		}
		return true;
	}

	std::optional<res::asset_id> root_guid_of(const json::value& document)
	{
		if (!document.is_object()) {
			return std::nullopt;
		}
		const json::value* guid = document.as_object().if_contains("__guid");
		if (!guid || !guid->is_string()) {
			return std::nullopt;
		}
		const auto id = res::asset_id_from_text(std::string_view(guid->as_string().data(), guid->as_string().size()));
		if (!id || id->is_nil()) {
			return std::nullopt;
		}
		return id;
	}

	// Rewrites the guid of every reference object ({"guid", "path"} and nothing else) that names an old id.
	bool rewrite_references(json::value& value, const id_map& fresh)
	{
		bool changed = false;
		if (value.is_array()) {
			for (json::value& element : value.as_array()) {
				changed = rewrite_references(element, fresh) || changed;
			}
			return changed;
		}
		if (!value.is_object()) {
			return false;
		}
		if (const auto reference = res::reference_from_json(value); reference && !reference->guid.is_nil()) {
			if (const auto it = fresh.find(reference->guid); it != fresh.end()) {
				value.as_object()["guid"] = res::to_text(it->second);
				return true;
			}
			return false;
		}
		for (auto& member : value.as_object()) {
			changed = rewrite_references(member.value(), fresh) || changed;
		}
		return changed;
	}
}

std::string edt::give_template_fresh_ids(const fs::path& project_root)
{
	const auto relative_name = [&project_root](const fs::path& file) { return utf8_of(file.lexically_relative(project_root)); };

	const prj::manifest_result manifest = prj::load_manifest(project_root / prj::k_manifest_file_name);
	if (!manifest.data) {
		return std::format("the new project's manifest is not usable: {}",
			manifest.error ? prj::to_string(*manifest.error) : std::string{ "unknown error" });
	}

	// Everything is listed before anything is written, so a sidecar made here is never taken for one of the template.
	std::set<fs::path> sidecars;
	std::set<fs::path> files;
	for (const fs::path& content_root : manifest.data->content_roots) {
		if (!is_plain_relative(content_root)) {
			continue;
		}
		const fs::path folder = (project_root / content_root).lexically_normal();
		std::error_code ec;
		if (!fs::is_directory(fs::symlink_status(folder, ec))) {
			continue;
		}
		fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec);
		for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			const fs::path& path = it->path();
			std::error_code status_ec;
			const fs::file_status status = it->symlink_status(status_ec);
			if (status_ec) {
				return std::format("cannot read '{}': {}", relative_name(path), status_ec.message());
			}
			if (is_hidden(path) || fs::is_symlink(status)) {
				if (fs::is_directory(status)) {
					it.disable_recursion_pending();
				}
				continue;
			}
			if (!fs::is_regular_file(status)) {
				continue;
			}
			(is_sidecar(path) ? sidecars : files).insert(path);
		}
		if (ec) {
			return std::format("cannot list '{}': {}", relative_name(folder), ec.message());
		}
	}

	std::map<fs::path, json::value> descs;
	for (const fs::path& file : files) {
		if (!is_desc_name(file)) {
			continue;
		}
		const auto text = read_capped(file, k_desc_max_bytes);
		if (!text) {
			egLOG_WARN("project", "the template desc '{}' cannot be read or is over 4 MiB: its references keep the template's ids",
				relative_name(file));
			continue;
		}
		json::parse_options options;
		options.max_depth = 128;
		boost::system::error_code ec;
		json::value document = json::parse(*text, ec, {}, options);
		if (ec) {
			egLOG_WARN("project", "the template desc '{}' is not valid JSON ({}): its references keep the template's ids",
				relative_name(file), ec.message());
			continue;
		}
		descs.emplace(file, std::move(document));
	}

	id_map fresh;
	for (const fs::path& sidecar : sidecars) {
		const auto text = read_capped(sidecar, k_sidecar_max_bytes);
		if (!text) {
			return std::format("the template's '{}' cannot be read or is over 64 KiB", relative_name(sidecar));
		}
		res::asset_meta_result parsed = res::read_asset_meta(*text);
		if (!parsed) {
			const auto& error = parsed.error;
			return std::format("the template's '{}' is not a valid .resource: {}{}{}", relative_name(sidecar), error.field_path,
				error.field_path.empty() ? "" : ": ", error.message);
		}
		res::asset_meta meta = std::move(*parsed.meta);
		const res::asset_id new_id = res::generate_asset_id();
		if (!fresh.emplace(meta.guid, new_id).second) {
			egLOG_WARN("project", "the template's '{}' repeats the id {}: references to it go to the first file that has it",
				relative_name(sidecar), res::to_text(meta.guid));
		}
		meta.guid = new_id;
		if (!write_text(sidecar, res::write_asset_meta(meta))) {
			return std::format("cannot write '{}'", relative_name(sidecar));
		}
	}

	for (const fs::path& file : files) {
		fs::path sidecar = file;
		sidecar += res::asset_meta_suffix;
		std::error_code ec;
		if (fs::exists(fs::symlink_status(sidecar, ec))) {
			continue;
		}
		const res::asset_id new_id = res::generate_asset_id();
		if (const auto desc = descs.find(file); desc != descs.end()) {
			// A desc's root __guid is its id (EPIC-57): the references that use it follow the file to its new id.
			if (const auto old_id = root_guid_of(desc->second)) {
				fresh.emplace(*old_id, new_id);
			}
		}
		if (!write_text(sidecar, res::write_asset_meta(res::asset_meta{ new_id, {}, {}, {} }))) {
			return std::format("cannot write '{}'", relative_name(sidecar));
		}
	}

	for (auto& [file, document] : descs) {
		bool changed = false;
		if (const auto old_id = root_guid_of(document)) {
			if (const auto it = fresh.find(*old_id); it != fresh.end()) {
				document.as_object()["__guid"] = res::to_text(it->second);
				changed = true;
			}
		}
		changed = rewrite_references(document, fresh) || changed;
		if (changed && !write_text(file, pretty(document))) {
			return std::format("cannot write '{}'", relative_name(file));
		}
	}
	return {};
}
