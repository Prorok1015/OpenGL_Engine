#include "edt_template_ids.h"
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/value.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_pretty_json.h>
#include <snk/resource/res_reference.h>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	constexpr std::uint64_t k_sidecar_max_bytes = 64u * 1024u;
	constexpr std::uint64_t k_desc_max_bytes = 4u * 1024u * 1024u;

	using id_map = std::unordered_map<res::asset_id, res::asset_id>;

	// Not path::string(): on Windows it goes through the ANSI code page and throws for what that cannot hold.
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.generic_u8string();
		return std::string(text.begin(), text.end());
	}

	std::string lowercase(std::string text)
	{
		for (char& c : text) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return text;
	}

	std::string name_of(const fs::path& path)
	{
		return utf8_of(path.filename());
	}

	bool is_desc_name(const fs::path& path)
	{
		return lowercase(name_of(path)).ends_with(".desc");
	}

	// The file a sidecar belongs to is found in any letter case of its name ("a.png.RESOURCE" is the sidecar of "a.png" on
	// every platform), so a file never gets a second sidecar next to one spelled differently.
	std::string owner_key(const fs::path& relative_file)
	{
		return utf8_of(relative_file.parent_path()) + "/" + lowercase(name_of(relative_file));
	}

	std::string owner_key_of_sidecar(const fs::path& relative_sidecar)
	{
		const std::string name = name_of(relative_sidecar);
		return utf8_of(relative_sidecar.parent_path()) + "/" + lowercase(name.substr(0, name.size() - res::asset_meta_suffix.size()));
	}

	std::string pretty(const json::value& value)
	{
		std::ostringstream os;
		res::write_pretty_json(os, value);
		return os.str();
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

	// A reference as the editor writes it: exactly the keys "guid" and "path".
	bool is_full_reference(const json::object& object)
	{
		return object.size() == 2 && object.contains("guid") && object.contains("path");
	}

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
		if (is_full_reference(value.as_object())) {
			const auto reference = res::reference_from_json(value);
			if (reference && !reference->guid.is_nil()) {
				if (const auto it = fresh.find(reference->guid); it != fresh.end()) {
					value.as_object()["guid"] = res::to_text(it->second);
					return true;
				}
			}
			return false;
		}
		for (auto& member : value.as_object()) {
			changed = rewrite_references(member.value(), fresh) || changed;
		}
		return changed;
	}

	// Every folder from the project root down to the content root is a real folder: the template is copied with its links
	// as they are, and a link there would take the walk and the writes out of the new project.
	std::string check_root_folders(const fs::path& project_root, const fs::path& content_root)
	{
		fs::path folder = project_root;
		for (const fs::path& part : content_root) {
			folder /= part;
			std::error_code ec;
			const fs::file_status status = fs::symlink_status(folder, ec);
			if (fs::is_symlink(status)) {
				return std::format("the content root '{}' passes through the link '{}': a new project is not made through a link",
					utf8_of(content_root), utf8_of(folder.lexically_relative(project_root)));
			}
			if (ec && ec != std::errc::no_such_file_or_directory) {
				return std::format("cannot read '{}': {}", utf8_of(folder.lexically_relative(project_root)), ec.message());
			}
		}
		return {};
	}

	struct read_outcome
	{
		std::string text;
		std::string error;
	};

	read_outcome read_template_file(const fs::path& project_root, const fs::path& relative, std::uint64_t max_bytes)
	{
		prj::project_file_text read = prj::read_project_file(project_root, relative, max_bytes);
		if (!read.error.empty()) {
			return { {}, std::format("the template's '{}' cannot be used: {}", utf8_of(relative), read.error) };
		}
		if (!read.exists) {
			return { {}, std::format("the template's '{}' is gone", utf8_of(relative)) };
		}
		return { std::move(read.text), {} };
	}

	std::string write_project_text(const fs::path& project_root, const fs::path& relative, std::string_view text)
	{
		if (std::optional<std::string> error = prj::write_project_file(project_root, relative, text)) {
			return std::move(*error);
		}
		return {};
	}
}

std::string edt::give_template_fresh_ids(const fs::path& project_root)
{
	const prj::manifest_result manifest = prj::load_manifest(project_root / prj::k_manifest_file_name);
	if (!manifest.data) {
		return std::format("the new project's manifest is not usable: {}",
			manifest.error ? prj::to_string(*manifest.error) : std::string{ "unknown error" });
	}

	// Relative to the project root. Everything is listed before anything is written, so a sidecar made here is never
	// taken for one of the template.
	std::set<fs::path> sidecars;
	std::set<fs::path> files;
	for (const fs::path& content_root : manifest.data->content_roots) {
		if (std::string error = check_root_folders(project_root, content_root); !error.empty()) {
			return error;
		}
		const fs::path folder = project_root / content_root;
		std::error_code ec;
		if (!fs::is_directory(fs::symlink_status(folder, ec))) {
			continue;
		}
		// The asset walk's rule: a content root "." is the template's folder, whose code/, build/ and manifest are no content.
		const bool project_folder = res::is_project_folder_layer(folder);
		fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec);
		for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			const fs::path relative = it->path().lexically_relative(project_root);
			std::error_code status_ec;
			const fs::file_status status = it->symlink_status(status_ec);
			if (status_ec) {
				return std::format("cannot read '{}': {}", utf8_of(relative), status_ec.message());
			}
			const std::string name = name_of(relative);
			const bool is_dir = fs::is_directory(status);
			if (res::is_skipped_asset_entry(name, is_dir, project_folder && it.depth() == 0)) {
				if (is_dir) {
					it.disable_recursion_pending();
				}
				continue;
			}
			if (is_dir || !fs::is_regular_file(status)) {
				continue;
			}
			(res::is_asset_meta_name(name) ? sidecars : files).insert(relative);
		}
		if (ec) {
			return std::format("cannot list '{}': {}", utf8_of(content_root), ec.message());
		}
	}

	std::map<fs::path, json::value> descs;
	for (const fs::path& file : files) {
		if (!is_desc_name(file)) {
			continue;
		}
		read_outcome read = read_template_file(project_root, file, k_desc_max_bytes);
		if (!read.error.empty()) {
			return read.error;
		}
		json::parse_options options;
		options.max_depth = 128;
		boost::system::error_code ec;
		json::value document = json::parse(read.text, ec, {}, options);
		if (ec) {
			return std::format("the template's '{}' is not valid JSON: {}", utf8_of(file), ec.message());
		}
		descs.emplace(file, std::move(document));
	}

	// Every file gets an id of its own; a reference to an old id follows the first file that had it.
	id_map fresh;
	std::unordered_map<std::string, res::asset_id> id_of_owner; // owner_key -> the file's new id
	for (const fs::path& sidecar : sidecars) {
		read_outcome read = read_template_file(project_root, sidecar, k_sidecar_max_bytes);
		if (!read.error.empty()) {
			return read.error;
		}
		res::asset_meta_result parsed = res::read_asset_meta(read.text);
		if (!parsed) {
			const auto& error = parsed.error;
			return std::format("the template's '{}' is not a valid .resource: {}{}{}", utf8_of(sidecar), error.field_path,
				error.field_path.empty() ? "" : ": ", error.message);
		}
		res::asset_meta meta = std::move(*parsed.meta);
		const res::asset_id new_id = res::generate_asset_id();
		if (!fresh.emplace(meta.guid, new_id).second) {
			egLOG_WARN("project", "the template's '{}' repeats the id {}: references to it go to the first file that has it",
				utf8_of(sidecar), res::to_text(meta.guid));
		}
		id_of_owner.emplace(owner_key_of_sidecar(sidecar), new_id);
		meta.guid = new_id;
		if (std::string error = write_project_text(project_root, sidecar, res::write_asset_meta(meta)); !error.empty()) {
			return error;
		}
	}

	for (const fs::path& file : files) {
		if (id_of_owner.contains(owner_key(file))) {
			continue;
		}
		const res::asset_id new_id = res::generate_asset_id();
		id_of_owner.emplace(owner_key(file), new_id);
		fs::path sidecar = file;
		sidecar += res::asset_meta_suffix;
		if (std::string error = write_project_text(project_root, sidecar, res::write_asset_meta(res::asset_meta{ new_id, {}, {}, {} }));
			!error.empty()) {
			return error;
		}
	}

	// A desc's root __guid is its id (EPIC-57): it becomes the new id of the desc's own sidecar, and a reference by the old
	// __guid follows the desc unless another file had that id first.
	for (const auto& [file, document] : descs) {
		if (const auto old_id = root_guid_of(document)) {
			fresh.emplace(*old_id, id_of_owner.at(owner_key(file)));
		}
	}

	for (auto& [file, document] : descs) {
		bool changed = false;
		if (document.is_object() && document.as_object().contains("__guid")) {
			const std::string own_id = res::to_text(id_of_owner.at(owner_key(file)));
			json::value& guid = document.as_object()["__guid"];
			if (!guid.is_string() || std::string_view(guid.as_string().data(), guid.as_string().size()) != own_id) {
				guid = own_id;
				changed = true;
			}
		}
		changed = rewrite_references(document, fresh) || changed;
		if (changed) {
			if (std::string error = write_project_text(project_root, file, pretty(document)); !error.empty()) {
				return error;
			}
		}
	}
	return {};
}
