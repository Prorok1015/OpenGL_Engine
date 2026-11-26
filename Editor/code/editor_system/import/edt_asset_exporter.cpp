#include "edt_asset_exporter.h"
#include "edt_process_decoder.h"
#include "edt_file_menu.h"
#include "edt_mesh_io.h"
#include "edt_model_import_meta.h"
#include "edt_version_notice.h"
#include <snk/common/fs_paths.h>
#include <snk/desc/desc_pretty_json.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/resource/res_desc_binary.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace json = boost::json;

namespace {

// Every string of `value` that names a file of an embedded texture, by its JSON pointer.
void embedded_texture_slots(const json::value& value, const std::string& pointer, std::map<std::string, std::string>& out, int depth = 0)
{
	if (depth > 64) return;
	if (value.is_string()) {
		const std::string_view text(value.as_string().data(), value.as_string().size());
		if (text.find("/__embedded_txm/") != std::string_view::npos) out.emplace(pointer, std::string(text));
	} else if (value.is_object()) {
		for (const auto& member : value.as_object()) {
			embedded_texture_slots(member.value(), pointer + "/" + std::string(member.key()), out, depth + 1);
		}
	} else if (value.is_array()) {
		const json::array& array = value.as_array();
		for (std::size_t i = 0; i < array.size(); ++i) embedded_texture_slots(array[i], pointer + "/" + std::to_string(i), out, depth + 1);
	}
}
	// Written with its binary file (EPIC-79), the desc on one line (#559).
	bool is_geometry_desc(std::string_view path)
	{
		return path.ends_with(".geom.desc");
	}

	// An empty folder is the root of res://.
	std::string folder_prefix(std::string_view target_folder)
	{
		return target_folder.empty() ? std::string{} : std::string(target_folder) + "/";
	}
}

edt::asset_exporter::asset_exporter(res::resource_system& res)
	: m_res(res)
{
}

std::unordered_map<res::tag, res::tag> edt::asset_exporter::build_remap(
	const import_result& result,
	const std::string& asset_name,
	const std::string& target_folder) const
{
	std::unordered_map<res::tag, res::tag> remap;
	std::string model_name = result.source_path.stem().string();
	// Every file of an import lies under the folder of its prefab: "<model>/" for an agent's, a folder of the run below
	// "__imports/" for a person's (#987, its name sanitized); the export keeps only what lies below that folder.
	std::string prefix = model_name + "/";
	if (const std::string_view root = result.root_tag.relative(); root.find('/') != std::string_view::npos) {
		const std::string_view folder = root.substr(0, root.rfind('/') + 1);
		if (folder == prefix || folder.starts_with("__imports/")) prefix = std::string(folder);
	}

	for (const auto& mem_tag : result.all_tags) {
		std::string rel = std::string(mem_tag.relative());

		// A person's run folder goes whatever the model's name (#1466): an empty stem leaves it in place otherwise.
		if ((!model_name.empty() || prefix.starts_with("__imports/")) && rel.starts_with(prefix))
			rel = rel.substr(prefix.size());

		// Replace the model name with the asset name where the importer put it: at the start of the file name, followed by a dot
		// (<model>.prefab.desc, <model>.geom.desc). Never inside a folder name or after the first dot: the name is the agent's
		// to choose, and a replacement in "x.raw" or "x.tga" could turn a texture into a file with a model's extension.
		auto replace_stem = [&](std::string& path, const std::string& old_name, const std::string& new_name) {
			if (old_name.empty())
				return;
			const size_t slash = path.rfind('/');
			const size_t file_start = slash == std::string::npos ? 0 : slash + 1;
			if (path.compare(file_start, old_name.size() + 1, old_name + ".") == 0)
				path.replace(file_start, old_name.size(), new_name);
		};

		if (asset_name != model_name)
			replace_stem(rel, model_name, asset_name);

		std::string res_path = folder_prefix(target_folder) + rel;
		res::tag res_tag = res::tag::make(res_path);
		remap[mem_tag] = res_tag;
	}

	return remap;
}

namespace {

void references_below(const json::value& value, std::string_view prefix, std::set<std::string>& out, int depth = 0)
{
	if (depth > 128) return;
	if (value.is_string()) {
		const std::string_view text(value.as_string().data(), value.as_string().size());
		if (text.starts_with(prefix)) out.emplace(text);
	} else if (value.is_object()) {
		for (const auto& member : value.as_object()) references_below(member.value(), prefix, out, depth + 1);
	} else if (value.is_array()) {
		for (const json::value& item : value.as_array()) references_below(item, prefix, out, depth + 1);
	}
}

}

edt::no_longer_made_list edt::asset_exporter::no_longer_made(const std::optional<made_files>& made,
	const std::optional<std::string>& old_prefab, std::string_view target_folder,
	const std::unordered_map<res::tag, res::tag>& remap)
{
	no_longer_made_list out;
	std::set<std::string> targets;
	for (const auto& [from, to] : remap) {
		targets.emplace(to.view());
		if (is_geometry_desc(to.relative())) {
			if (const std::optional<res::tag> binary = res::desc_binary::binary_tag(to)) {
				targets.emplace(binary->view());
			}
		}
	}
	const std::string prefix =
		std::format("{}://{}", res::tag::default_protocol(), folder_prefix(target_folder));
	std::set<std::string> left;
	if (made) {
		for (const auto& [path, sha256] : *made) left.emplace(prefix + path);
	} else if (!old_prefab) {
		out.note = "the earlier prefab could not be read: files it made are not listed";
		return out;
	} else if (target_folder.empty()) {
		out.note = "the files of the earlier import are not known (old record)";
		return out;
	} else {
		json::parse_options opts;
		opts.max_depth = 128;
		boost::system::error_code ec;
		const json::value old = json::parse(*old_prefab, ec, {}, opts);
		if (ec) {
			out.note = "the earlier prefab could not be read: files it made are not listed";
			return out;
		}
		references_below(old, prefix, left);
	}
	for (const std::string& target : targets) left.erase(target);
	out.tags.reserve(left.size());
	for (const std::string& text : left) out.tags.emplace_back(text);
	return out;
}

std::string edt::asset_exporter::folder_problem(std::string_view target_folder)
{
	if (target_folder.empty()) return {};
	if (target_folder.find("://") != std::string_view::npos) {
		return std::format("'{}': write the folder as a path inside res:// (\"assets/tree\"), without a protocol", target_folder);
	}
	if (agent_level_path checked = check_agent_folder(target_folder); !checked.ok()) {
		return std::format("target folder: {}", checked.error);
	}
	return {};
}

std::string edt::asset_exporter::asset_name_problem(std::string_view asset_name)
{
	if (asset_name.empty()) return "enter an asset name";
	if (agent_level_path checked = check_agent_name(asset_name); !checked.ok()) {
		return std::format("asset name: {}", checked.error);
	}
	return {};
}

std::string edt::asset_exporter::destination_problem(const std::unordered_map<res::tag, res::tag>& remap, std::string_view asset_name, std::string_view target_folder)
{
	if (std::string name = asset_name_problem(asset_name); !name.empty()) {
		return name;
	}
	if (std::string folder = folder_problem(target_folder); !folder.empty()) {
		return folder;
	}
	const std::string prefix = folder_prefix(target_folder);
	for (const auto& [from, to] : remap) {
		if (to.protocol() != res::tag::default_protocol() || !to.relative().starts_with(prefix)) {
			return std::format("'{}' would be written outside the folder of the import", to.view());
		}
		// The names inside the folder come from the model file: they are held to the same rules as the folder.
		if (agent_level_path checked = check_agent_folder(to.relative()); !checked.ok()) {
			return std::format("'{}': {}", to.view(), checked.error);
		}
	}
	return {};
}

std::string edt::asset_exporter::remap_problem(const std::unordered_map<res::tag, res::tag>& remap, std::string_view asset_name, std::string_view target_folder)
{
	if (std::string destination = destination_problem(remap, asset_name, target_folder); !destination.empty()) {
		return destination;
	}
	const std::string prefix = folder_prefix(target_folder);
	for (const auto& [from, to] : remap) {
		const std::string_view final_path = to.relative();
		const std::string_view relative = final_path.substr(prefix.size());
		// What lands in the project is what an import makes, under the names the importer would give it, and the remap changed
		// no extension: the adapter reads a file by its extension.
		if (!is_import_resource_name(relative, asset_name)) {
			return std::format("'{}' is not a name an import gives a resource", to.view());
		}
		const std::string_view before = from.relative();
		if (file_extension_lowercase(before) != file_extension_lowercase(final_path)) {
			return std::format("the export would change the extension of '{}'", from.view());
		}
	}
	return {};
}

std::string edt::asset_exporter::export_geometry(const res::tag& from, const res::tag& to, const json::object& desc,
	std::vector<res::tag>* written, bool& wrote, std::vector<res::tag>& on_disk) const
{
	wrote = false;
	on_disk.clear();
	rnd::geometry_file_options options;
	options.topology = rnd::topology_of(desc);
	if (options.topology != rnd::geometry_topology::triangles) {
		// Lines and points have no binary file: their arrays go as they are, on one line (#559), as before EPIC-79.
		const std::string text = json::serialize(desc);
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		if (unchanged_on_disk(to, bytes)) {
			on_disk.push_back(to);
			return {};
		}
		if (!m_res.store_sync(to, bytes)) {
			return std::format("'{}' could not be written (see the log)", to.view());
		}
		if (written) written->push_back(to);
		on_disk.push_back(to);
		wrote = true;
		return {};
	}

	rnd::geometry_desc geometry;
	if (std::string problem = rnd::read_geometry_file(m_res, from, desc, geometry); !problem.empty()) {
		return std::format("'{}' could not be read: {}", from.view(), problem);
	}
	for (const auto& member : desc) {
		options.header[member.key()] = member.value();
	}
	options.bounds = !geometry.bounds.empty();
	// Refused data (an index past the vertices, a position that is not finite, ...) ends the export, naming the check.
	const rnd::geometry_files files = rnd::encode_geometry_files(to, geometry, options);
	if (!files) {
		return std::format("'{}' cannot be written: {}", to.view(), files.error);
	}
	const res::tag binary = *res::desc_binary::binary_tag(to);
	if (unchanged_on_disk(to, files.desc) && unchanged_on_disk(binary, files.binary)) {
		on_disk = { to, binary };
		return {};
	}

	// An old project's mesh in JSON arrays is converted at this save (owner decision, #1068).
	const bool had_arrays = mesh::file_holds_geometry_arrays(m_res, to);
	// A failed write names what it left (the staged file, or the desc with it), so whoever takes the export back takes
	// exactly that; a desc never goes without the staged file it needs.
	if (std::string problem = rnd::write_geometry_files(m_res, to, geometry, options, written); !problem.empty()) {
		return problem;
	}
	if (had_arrays) {
		egLOG_WARN("Editor/Save", "{}", geometry_conversion_text(to.view(), binary.view()));
	}
	if (written) {
		written->push_back(to);
		written->push_back(binary);
	}
	on_disk = { to, binary };
	wrote = true;
	return {};
}

bool edt::asset_exporter::unchanged_on_disk(const res::tag& tag, const std::vector<std::byte>& bytes) const
{
	if (!m_res.exists(tag)) return false;
	const auto current = m_res.fetch_data(tag);
	if (!current) return false;
	const std::vector<std::byte>& on_disk = current->get();
	return !current->has_error() && on_disk == bytes;
}

std::vector<std::string> edt::asset_exporter::replaced_embedded_textures(const res::tag& to, const json::value& desc) const
{
	if (!to.relative().ends_with(".mat.desc") || !m_res.exists(to)) return {};
	const auto current = m_res.fetch_data(to);
	if (!current || current->has_error()) return {};
	const std::vector<std::byte>& bytes = current->get();
	json::value old;
	try {
		json::parse_options opts;
		opts.max_depth = 128;
		old = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {}, opts);
	} catch (const std::exception&) {
		return {};
	}
	std::map<std::string, std::string> before;
	std::map<std::string, std::string> after;
	embedded_texture_slots(old, "", before);
	embedded_texture_slots(desc, "", after);
	std::vector<std::string> out;
	for (const auto& [slot, name] : before) {
		const auto now = after.find(slot);
		if (now != after.end() && now->second == name) continue;
		out.push_back(std::format("{} {}: {} -> {}", to.view(), slot, name, now == after.end() ? std::string("(none)") : now->second));
	}
	return out;
}

void edt::asset_exporter::remap_json_strings(
	json::value& val,
	const std::unordered_map<res::tag, res::tag>& remap)
{
	if (val.is_string()) {
		std::string s(val.as_string().c_str(), val.as_string().size());
		// Check if this string looks like a memory:// tag
		if (s.starts_with("memory://")) {
			res::tag mem_tag{ s };
			if (auto it = remap.find(mem_tag); it != remap.end()) {
				val = std::string(it->second.view());
			}
		}
	} else if (val.is_object()) {
		for (auto& kv : val.as_object()) {
			remap_json_strings(kv.value(), remap);
		}
	} else if (val.is_array()) {
		for (auto& elem : val.as_array()) {
			remap_json_strings(elem, remap);
		}
	}
}

std::optional<std::vector<std::byte>> edt::asset_exporter::planned_bytes(const res::tag& mem_tag,
	const std::unordered_map<res::tag, res::tag>& remap, std::string& error) const
{
	error.clear();
	const std::string_view rel = mem_tag.relative();
	if (is_geometry_desc(rel)) return std::nullopt;
	auto data_block = m_res.fetch_data(mem_tag);
	if (!data_block) {
		error = std::format("'{}' could not be read", mem_tag.view());
		return std::nullopt;
	}
	const std::vector<std::byte>& bytes = data_block->get();
	if (data_block->has_error()) {
		error = std::format("'{}' could not be read", mem_tag.view());
		return std::nullopt;
	}
	if (rel.find(".desc") == std::string_view::npos) return bytes;
	try {
		json::parse_options opts;
		opts.max_depth = 128;
		const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		json::value val = json::parse(text, {}, opts);
		remap_json_strings(val, remap);
		return desc::pretty_json_bytes(val);
	} catch (const std::exception& e) {
		// Written as they are, its memory:// references would point at nothing once the editor closes.
		error = std::format("'{}' is not valid JSON: {}", mem_tag.view(), e.what());
		return std::nullopt;
	}
}

namespace {

edt::edit_state worse(edt::edit_state a, edt::edit_state b)
{
	const auto rank = [](edt::edit_state state) {
		return state == edt::edit_state::changed ? 2 : state == edt::edit_state::unknown ? 1 : 0;
	};
	return rank(a) >= rank(b) ? a : b;
}

}

edt::reimport_scan edt::asset_exporter::scan_reimport(const std::unordered_map<res::tag, res::tag>& remap,
	const std::filesystem::path& top, std::string_view target_folder, const std::optional<made_files>& made,
	ds::task_context* ctx) const
{
	reimport_scan out;
	const std::string prefix = folder_prefix(target_folder);
	const auto recorded = [&](const res::tag& tag) -> const std::string* {
		const std::string_view rel = tag.relative();
		if (!made || tag.protocol() != res::tag::default_protocol() || !rel.starts_with(prefix)) return nullptr;
		const auto found = made->find(std::string(rel.substr(prefix.size())));
		return found == made->end() ? nullptr : &found->second;
	};
	std::vector<std::pair<res::tag, res::tag>> sorted(remap.begin(), remap.end());
	std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.view() < b.second.view(); });
	out.files.reserve(sorted.size());
	for (const auto& [from, to] : sorted) {
		if (ctx) ctx->throw_if_stopped();
		reimport_file_check check;
		check.tag = to;
		check.state = edit_state::unchanged;
		std::vector<res::tag> files{ to };
		if (is_geometry_desc(to.relative())) {
			if (const std::optional<res::tag> binary = res::desc_binary::binary_tag(to)) files.push_back(*binary);
		}
		std::string sha256;
		for (const res::tag& file : files) {
			const project_file_hash hash = hash_project_file(top, file);
			if (hash.status == project_file_hash::state::missing) {
				out.seen[file] = {};
				continue;
			}
			if (hash.status != project_file_hash::state::ok) {
				if (out.error.empty()) out.error = hash.error;
				check.exists = true;
				check.state = edit_state::unknown;
				continue;
			}
			out.seen[file] = hash.sha256;
			check.exists = true;
			if (file == to) sha256 = hash.sha256;
			const std::string* was = recorded(file);
			check.state = worse(check.state, !was ? edit_state::unknown
				: *was == hash.sha256 ? edit_state::unchanged : edit_state::changed);
		}
		if (check.exists && !sha256.empty()) {
			std::string why;
			// A geometry pair is written by rnd::write_geometry_files: its bytes are not compared here.
			if (const std::optional<std::vector<std::byte>> planned = planned_bytes(from, remap, why)) {
				check.same_as_new = sha256_hex(*planned) == sha256;
			}
		}
		if (!check.exists) check.state = edit_state::unchanged;
		out.files.push_back(std::move(check));
	}
	return out;
}

bool edt::asset_exporter::export_to_project(
	const import_result& result,
	const std::unordered_map<res::tag, res::tag>& remap,
	export_progress* progress,
	ds::task_context* ctx,
	std::vector<res::tag>* written,
	bool wait_for_writes,
	const std::function<void(int, int)>& on_file,
	const std::filesystem::path& meta_top,
	bool may_replace_meta,
	const std::unordered_map<res::tag, std::string>* seen) const
{
	// Process tags in order: leaves first (textures, geometry), then prefab (root) last.
	// root_tag is already known from import_result — no filename guessing needed.
	std::vector<res::tag> ordered_tags;

	for (const auto& tag : result.all_tags) {
		if (tag == result.root_tag)
			continue; // add last
		ordered_tags.push_back(tag);
	}
	if (result.root_tag.is_valid())
		ordered_tags.push_back(result.root_tag);

	if (progress) {
		progress->total.store(static_cast<int>(ordered_tags.size()));
		progress->done.store(0);
	}

	// A file left out would leave a reference to it in the prefab: the export stops there instead of reporting success.
	const auto fail = [progress](std::string why) {
		egLOG_ERROR("edt/export", "{}", why);
		if (progress) {
			progress->error = std::move(why);
			progress->finished.store(true);
		}
		return false;
	};

	// The prefab's `.resource` (#1195) is checked before any file is written: a refusal leaves the project as it was.
	std::optional<model_meta_plan> meta;
	const std::filesystem::path top = meta_top.empty() ? m_res.get_resources_path() : meta_top;
	std::string made_prefix;
	if (!result.source_sha256.empty() && result.root_tag.is_valid()) {
		const auto root = remap.find(result.root_tag);
		if (root == remap.end()) {
			return fail(std::format("'{}' has no place in the project", result.root_tag.view()));
		}
		const model_source_record source =
			model_source_of(fs_paths::utf8_of(result.source_path.filename()), result.source_sha256);
		meta = plan_model_meta(top, root->second, result.settings, source, may_replace_meta);
		if (!meta->ok()) return fail(meta->error);
		const std::string_view prefab = root->second.relative();
		made_prefix = std::string(prefab.substr(0, prefab.rfind('/') + 1));
	}

	// What the export leaves on the disk, for the record of the prefab (#1194).
	made_files made;
	const auto made_key = [&](const res::tag& tag) -> std::optional<std::string> {
		const std::string_view rel = tag.relative();
		if (!meta || tag.protocol() != res::tag::default_protocol() || !rel.starts_with(made_prefix)) {
			return std::nullopt;
		}
		return std::string(rel.substr(made_prefix.size()));
	};
	// A re-import overwrites only bytes its check showed the person.
	const auto changed_since_seen = [&](const res::tag& tag) -> std::string {
		if (!seen) return {};
		const auto was = seen->find(tag);
		if (was == seen->end()) return {};
		const project_file_hash now = hash_project_file(top, tag);
		if (now.status == project_file_hash::state::missing) return {};
		if (now.status == project_file_hash::state::ok && now.sha256 == was->second) return {};
		return std::format("{} changed after the dialog looked at it: nothing more was written", tag.view());
	};

	int files_done = 0;
	int files_written = 0;
	for (const auto& mem_tag : ordered_tags) {
		if (ctx) ctx->throw_if_stopped();
		auto remap_it = remap.find(mem_tag);
		if (remap_it == remap.end()) {
			return fail(std::format("'{}' has no place in the project", mem_tag.view()));
		}

		const res::tag& res_tag = remap_it->second;

		if (mem_tag != result.root_tag && !m_res.exists(mem_tag)) {
			egLOG_WARN("edt/export", "'{}' is not in the import (the importer did not find it): left out", mem_tag.view());
			if (progress) {
				progress->skipped.push_back(res_tag);
				progress->done.fetch_add(1);
			}
			continue;
		}

		std::string rel(mem_tag.relative());

		if (is_geometry_desc(rel)) {
			auto data_block = m_res.fetch_data(mem_tag);
			if (!data_block) {
				return fail(std::format("'{}' could not be read", mem_tag.view()));
			}
			auto& bytes = data_block->get();
			if (data_block->has_error()) {
				return fail(std::format("'{}' could not be read", mem_tag.view()));
			}
			json::value val;
			try {
				json::parse_options opts;
				opts.max_depth = 128;
				val = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {}, opts);
				remap_json_strings(val, remap);
			} catch (const std::exception& e) {
				return fail(std::format("'{}' is not valid JSON: {}", mem_tag.view(), e.what()));
			}
			if (!val.is_object()) {
				return fail(std::format("'{}' is not a JSON object", mem_tag.view()));
			}
			if (std::string why = changed_since_seen(res_tag); !why.empty()) return fail(std::move(why));
			if (const std::optional<res::tag> binary = res::desc_binary::binary_tag(res_tag)) {
				if (std::string why = changed_since_seen(*binary); !why.empty()) return fail(std::move(why));
			}
			bool wrote = false;
			std::vector<res::tag> on_disk;
			if (std::string problem = export_geometry(mem_tag, res_tag, val.as_object(), written, wrote, on_disk);
				!problem.empty()) {
				return fail(std::move(problem));
			}
			for (const res::tag& file : on_disk) {
				const std::optional<std::string> key = made_key(file);
				if (!key) continue;
				const project_file_hash hash = hash_project_file(top, file);
				if (hash.status == project_file_hash::state::ok) {
					made[*key] = hash.sha256;
				} else {
					egLOG_WARN("edt/export", "{} is not recorded as made by the import: {}", file.view(),
						hash.error.empty() ? std::string("it is not on the disk") : hash.error);
				}
			}
			if (wrote) {
				++files_written;
			} else {
				egLOG_INFO("edt/export", "Unchanged: {}", res_tag.view());
			}
			if (progress) progress->done.fetch_add(1);
			++files_done;
			if (on_file) on_file(files_done, static_cast<int>(ordered_tags.size()));
			egLOG_INFO("edt/export", "Exported: {} → {} (with its binary file)", mem_tag.view(), res_tag.view());
			continue;
		}

		std::string unreadable;
		std::optional<std::vector<std::byte>> planned = planned_bytes(mem_tag, remap, unreadable);
		if (!planned) return fail(std::move(unreadable));
		const std::vector<std::byte>& output_bytes = *planned;
		if (res_tag.relative().ends_with(".mat.desc")) {
			json::parse_options opts;
			opts.max_depth = 128;
			boost::system::error_code ec;
			const std::string_view text(reinterpret_cast<const char*>(output_bytes.data()), output_bytes.size());
			const json::value val = json::parse(text, ec, {}, opts);
			for (std::string& line : ec ? std::vector<std::string>{} : replaced_embedded_textures(res_tag, val)) {
				egLOG_WARN("edt/export", "the material slot {} now names another embedded texture; the old file stays "
					"in the project (point other references at the new one, then remove the old with "
					"asset.delete)", line);
				if (progress) progress->replaced_textures.push_back(std::move(line));
			}
		}
		if (std::string why = changed_since_seen(res_tag); !why.empty()) return fail(std::move(why));
		if (const std::optional<std::string> key = made_key(res_tag)) made[*key] = sha256_hex(output_bytes);

		// A new sidecar goes before the prefab, so the database adopts its guid; one being replaced already holds the
		// guid, so it follows the prefab it describes (re-import, #1194).
		const bool with_meta = meta && mem_tag == result.root_tag;
		const bool meta_first = with_meta && !meta->before.has_value();
		if (with_meta) {
			if (std::string why = set_made(*meta, made); !why.empty()) {
				egLOG_WARN("edt/export", "{} records no files of the import: {}", meta->tag.view(), why);
			}
		}
		if (meta_first) {
			if (std::string error = write_model_meta(top, *meta); !error.empty()) return fail(std::move(error));
			if (written) written->push_back(meta->tag);
		}
		const auto prefab_failed = [&](std::string why) {
			if (meta_first) {
				if (std::string left = take_back_model_meta(top, *meta); !left.empty()) {
					why += "; " + left;
				} else if (written && !written->empty() && written->back() == meta->tag) {
					written->pop_back();
				}
			}
			return fail(std::move(why));
		};

		if (unchanged_on_disk(res_tag, output_bytes)) {
			egLOG_INFO("edt/export", "Unchanged: {}", res_tag.view());
		} else {
			// Write via VFS — store() handles disk write + mark_written to suppress file_watcher.
			if (wait_for_writes) {
				if (!m_res.store_sync(res_tag, output_bytes)) {
					return prefab_failed(std::format("'{}' could not be written (see the log)", res_tag.view()));
				}
			} else {
				m_res.store(res_tag, output_bytes);
			}
			if (written) written->push_back(res_tag);
			++files_written;
		}

		if (with_meta && !meta_first) {
			if (std::string error = write_model_meta(top, *meta); !error.empty()) {
				return fail(std::format("{}; the prefab is written, its .resource still records the earlier "
					"settings", error));
			}
			if (written) written->push_back(meta->tag);
		}

		if (progress) progress->done.fetch_add(1);
		++files_done;
		if (on_file) on_file(files_done, static_cast<int>(ordered_tags.size()));

		egLOG_INFO("edt/export", "Exported: {} → {}", mem_tag.view(), res_tag.view());
	}

	// Store the exported root tag
	if (progress) {
		if (auto it = remap.find(result.root_tag); it != remap.end())
			progress->exported_tag = it->second;
		progress->success.store(true);
		progress->finished.store(true);
	}

	egLOG_INFO("edt/export", "Export complete: {} files written, {} unchanged", files_written, files_done - files_written);
	return true;
}
