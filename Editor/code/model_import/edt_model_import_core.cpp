#include "edt_model_import_core.h"
#include "edt_model_file_source.h"

#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_pretty_json.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

void checkpoint(ds::task_context* ctx)
{
	if (ctx) ctx->throw_if_stopped();
}

// How the log names the model: its file name, never the folder it lies in (agents read the log).
std::string display_name_of(const std::filesystem::path& path)
{
	return fs_paths::utf8_of(path.filename());
}

mdl::import_options read_options(const edt::import_source& source, ds::task_context* ctx)
{
	mdl::import_options options;
	options.display_name = display_name_of(source.path);
	options.task = ctx;
	options.deadline = source.deadline;
	return options;
}

} // anonymous namespace

edt::model_file_scan edt::scan_model_files(const import_source& source, ds::task_context* ctx)
{
	model_file_scan scan;
	if (!source.preloaded) {
		scan.error = "the model file was not read";
		return scan;
	}
	model_file_access access(source.path, source.limits, nullptr, source.boundary, source.on_disk);
	model_file_source files(source.path, *source.preloaded, &access);
	const mdl::referenced_files found = mdl::list_referenced_files(source.path.string(), files, read_options(source, ctx));
	if (found.failed == mdl::failure::timed_out) {
		scan.error = found.error;
		return scan;
	}
	if (found.failed != mdl::failure::none) {
		scan.error = std::format("the model cannot be read: {}", found.error);
		return scan;
	}
	for (const std::string& name : found.textures) {
		const resolved_reference reference = access.resolve(name, model_file_kind::texture);
		if (!reference.error.empty()) {
			scan.error = reference.error;
			return scan;
		}
		checkpoint(ctx);
		if (mdl::past_deadline(source.deadline)) {
			scan.error = "the model took too long to read";
			return scan;
		}
		try {
			access.hash_listed(reference);
		} catch (const std::runtime_error& e) {
			// Not an image, changed while read, over the budget: the bytes are checked here, not by resolve().
			scan.error = e.what();
			return scan;
		}
	}
	scan.files = access.files();
	scan.total_bytes = access.listed_bytes();
	return scan;
}

edt::scan_requests edt::list_model_requests(const import_source& source, ds::task_context* ctx)
{
	scan_requests out;
	if (!source.preloaded) {
		out.error = "the model file was not read";
		return out;
	}
	const std::vector<provided_file> none;
	model_file_access access(source.path, source.limits, source.provided ? source.provided : &none, model_file_access::provided_mode::record);
	model_file_source files(source.path, *source.preloaded, &access);
	const mdl::referenced_files found = mdl::list_referenced_files(source.path.string(), files, read_options(source, ctx));
	if (found.failed == mdl::failure::timed_out) {
		out.error = found.error;
		return out;
	}
	const auto& requested = access.requested();
	if (std::any_of(requested.begin(), requested.end(), [](const requested_file& file) { return file.kind == model_file_kind::side_file; })) {
		out.requested = requested;
		return out;
	}
	if (found.failed != mdl::failure::none) {
		out.error = std::format("the model cannot be read: {}", found.error);
		return out;
	}
	for (const std::string& name : found.textures) {
		const resolved_reference reference = access.resolve(name, model_file_kind::texture);
		if (!reference.error.empty()) {
			out.error = reference.error;
			return out;
		}
		checkpoint(ctx);
		if (mdl::past_deadline(source.deadline)) {
			out.error = "the model took too long to read";
			return out;
		}
	}
	out.requested = access.requested();
	return out;
}

edt::import_intermediate edt::import_model_into(const import_source& source, mdl::import_sink& sink, ds::task_context* ctx,
	const std::function<void(boost::json::object&)>& stamp)
{
	const std::filesystem::path& abs_path = source.path;
	import_intermediate im;
	im.source_path = abs_path;
	if (!source.on_disk && (!(source.allowed || source.provided) || !source.preloaded)) {
		// Without model_file_access the importer reads side files from the folder of `path`, which names no folder here.
		im.error = "a model that is not a file on the disk is imported from its bytes with the list of files it may read";
		return im;
	}
	std::optional<model_file_access> access;
	if (source.provided) {
		access.emplace(abs_path, source.limits, source.provided, model_file_access::provided_mode::strict);
	} else if (source.allowed) {
		access.emplace(abs_path, source.limits, source.allowed, source.boundary, source.on_disk);
	}
	model_file_access* const files_access = access ? &*access : nullptr;
	checkpoint(ctx);

	// An agent's import brings the bytes it checked.
	std::vector<std::byte> file_data;
	if (source.preloaded) {
		file_data = *source.preloaded;
	} else {
		std::ifstream in(abs_path, std::ios::binary | std::ios::ate);
		if (!in) {
			im.error = std::format("Failed to open file: {}", abs_path.string());
			egLOG_ERROR("edt/import", "{}", im.error);
			return im;
		}

		const auto file_size = static_cast<std::uintmax_t>(in.tellg());
		if (file_size > k_max_person_source_bytes) {
			im.error = std::format("The model file is too big: {} bytes, the limit is {}", file_size, k_max_person_source_bytes);
			egLOG_ERROR("edt/import", "{}", im.error);
			return im;
		}
		in.seekg(0);
		file_data.resize(static_cast<size_t>(file_size));
		constexpr std::size_t chunk = 8ull * 1024ull * 1024ull;
		for (std::size_t done = 0; done < file_data.size(); done += chunk) {
			checkpoint(ctx);
			const std::size_t part = (std::min)(chunk, file_data.size() - done);
			if (!in.read(reinterpret_cast<char*>(file_data.data() + done), static_cast<std::streamsize>(part))) {
				im.error = std::format("Failed to read file: {}", abs_path.string());
				egLOG_ERROR("edt/import", "{}", im.error);
				return im;
			}
		}
	}
	checkpoint(ctx);

	// Everything under memory://<memory_folder> ("<stem>/" by default): textures/, materials/, __embedded_txm/.
	const std::string model_name = abs_path.stem().string();
	const std::string mem_prefix = source.memory_folder.empty() ? model_name + "/" : source.memory_folder;
	mdl::import_options options = read_options(source, ctx);
	options.geometry_tag = res::tag{ res::tag::memory, mem_prefix + model_name + ".geom.desc" };
	options.prefab_tag = res::tag{ res::tag::memory, mem_prefix + model_name + ".prefab.desc" };
	options.embedded_folder = mem_prefix + "__embedded_txm/";
	options.texture_folder = mem_prefix + "textures/";
	options.material_folder = mem_prefix + "materials/";
	options.materials = mdl::material_mode::separate_files;
	options.textures = mdl::texture_mode::copy;
	im.geom_tag = options.geometry_tag;

	model_file_source files(abs_path, file_data, files_access);
	mdl::import_result imported = mdl::import_model(abs_path.string(), files, sink, options);
	switch (imported.failed) {
	case mdl::failure::none:
		break;
	case mdl::failure::timed_out:
		im.error = imported.error;
		return im;
	case mdl::failure::parse:
		im.error = std::format("ASSIMP error: {}", imported.error);
		egLOG_ERROR("edt/import", "{}", im.error);
		return im;
	case mdl::failure::geometry:
		im.error = std::format("the geometry of the model is refused: {}", imported.error);
		egLOG_ERROR("edt/import", "{}", im.error);
		return im;
	case mdl::failure::too_large:
		im.error = std::format("the model is too large: {}", imported.error);
		egLOG_ERROR("edt/import", "{}", im.error);
		return im;
	}
	checkpoint(ctx);

	// The prefab, stamped by the caller's desc system when it has one (desc_system::serialize_to_bytes is this pretty JSON).
	// The geometry's binary file is not in all_tags: whoever copies the desc carries it along (asset_exporter).
	boost::json::object jsdata = std::move(imported.prefab);
	if (stamp) stamp(jsdata);
	im.prefab_tag = options.prefab_tag;
	sink.store(im.prefab_tag, desc::pretty_json_bytes(jsdata));
	im.prefab_json = std::move(jsdata);
	im.all_tags = std::move(imported.tags);
	im.all_tags.push_back(im.prefab_tag);
	{
		std::unordered_set<res::tag> seen;
		std::erase_if(im.all_tags, [&seen](const res::tag& t) { return !seen.insert(t).second; });
	}
	im.skin_weights = std::move(imported.skin_weights);
	im.missing_textures = std::move(imported.missing_textures);
	im.missing_textures_not_listed = imported.missing_textures_not_listed;

	egLOG("edt/import", "Background import done: '{}' → {} memory resources", display_name_of(abs_path), im.all_tags.size());
	for (const auto& t : im.all_tags) {
		egLOG("edt/import", "  tag: {}", t.view());
	}
	return im;
}
