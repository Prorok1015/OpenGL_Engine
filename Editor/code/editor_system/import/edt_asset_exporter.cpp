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
#include <cstring>
#include <format>
#include <map>
#include <optional>
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

	for (const auto& mem_tag : result.all_tags) {
		std::string rel = std::string(mem_tag.relative());

		// Strip the model_name/ prefix from relative path
		std::string prefix = model_name + "/";
		if (!model_name.empty() && rel.starts_with(prefix))
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
	std::vector<res::tag>* written, bool& wrote) const
{
	wrote = false;
	rnd::geometry_file_options options;
	options.topology = rnd::topology_of(desc);
	if (options.topology != rnd::geometry_topology::triangles) {
		// Lines and points have no binary file: their arrays go as they are, on one line (#559), as before EPIC-79.
		const std::string text = json::serialize(desc);
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		if (unchanged_on_disk(to, bytes)) {
			return {};
		}
		if (!m_res.store_sync(to, bytes)) {
			return std::format("'{}' could not be written (see the log)", to.view());
		}
		if (written) written->push_back(to);
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

bool edt::asset_exporter::export_to_project(
	const import_result& result,
	const std::unordered_map<res::tag, res::tag>& remap,
	export_progress* progress,
	ds::task_context* ctx,
	std::vector<res::tag>* written,
	bool wait_for_writes,
	const std::function<void(int, int)>& on_file,
	const std::filesystem::path& meta_top,
	bool may_replace_meta) const
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
	if (!result.source_sha256.empty() && result.root_tag.is_valid()) {
		const auto root = remap.find(result.root_tag);
		if (root == remap.end()) {
			return fail(std::format("'{}' has no place in the project", result.root_tag.view()));
		}
		const model_source_record source{ fs_paths::utf8_of(result.source_path.filename()), result.source_sha256 };
		meta = plan_model_meta(top, root->second, result.settings, source, may_replace_meta);
		if (!meta->ok()) return fail(meta->error);
	}

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

		auto data_block = m_res.fetch_data(mem_tag);
		if (!data_block) {
			return fail(std::format("'{}' could not be read", mem_tag.view()));
		}
		auto& bytes = data_block->get();
		if (data_block->has_error()) {
			return fail(std::format("'{}' could not be read", mem_tag.view()));
		}

		std::vector<std::byte> output_bytes;
		std::string rel(mem_tag.relative());

		if (is_geometry_desc(rel)) {
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
			bool wrote = false;
			if (std::string problem = export_geometry(mem_tag, res_tag, val.as_object(), written, wrote); !problem.empty()) {
				return fail(std::move(problem));
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

		// If it's a .desc file, parse JSON and remap memory:// references
		if (rel.find(".desc") != std::string::npos) {
			std::string json_str(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			try {
				json::parse_options opts;
				opts.max_depth = 128;
				json::value val = json::parse(json_str, {}, opts);
				remap_json_strings(val, remap);
				output_bytes = desc::pretty_json_bytes(val);
				for (std::string& line : replaced_embedded_textures(res_tag, val)) {
					egLOG_WARN("edt/export", "the material slot {} now names another embedded texture; the old file stays in the "
						"project (point other references at the new one, then remove the old with asset.delete)", line);
					if (progress) progress->replaced_textures.push_back(std::move(line));
				}
			} catch (const std::exception& e) {
				// Written as they are, its memory:// references would point at nothing once the editor closes.
				return fail(std::format("'{}' is not valid JSON: {}", mem_tag.view(), e.what()));
			}
		} else {
			output_bytes = bytes;
		}

		// Before the prefab: the asset database, seeing it, finds its ID and makes none.
		const bool with_meta = meta && mem_tag == result.root_tag;
		if (with_meta) {
			if (std::string error = write_model_meta(top, *meta); !error.empty()) return fail(std::move(error));
			if (written) written->push_back(meta->tag);
		}
		const auto prefab_failed = [&](std::string why) {
			if (with_meta) {
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
