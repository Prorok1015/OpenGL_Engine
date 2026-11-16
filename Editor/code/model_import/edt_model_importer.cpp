#include "edt_model_importer.h"
#include "edt_model_file_source.h"

#include <snk/model_import/mdl_import.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <unordered_set>

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

// What the editor's import writes, over the resource system: `all` gets every tag the import refers to, `created` the
// ones this run stored itself because they did not exist yet; only those may be erased again (the others belong to an
// earlier import of the same model). With `files` (an agent's import) nothing already in memory:// is reused.
class tracking_sink final : public mdl::import_sink
{
public:
	tracking_sink(res::resource_system& res, std::vector<res::tag>& created, edt::model_file_access* files)
		: m_res(res), m_created(created), m_files(files)
	{
	}

	// True when the tag was stored by this run before. A tag from anywhere else is not this import's to reuse or
	// overwrite in an agent's import: its bytes could belong to another model.
	bool exists(const res::tag& tag) override
	{
		if (!m_res.exists(tag)) return false;
		if (m_files && std::find(m_created.begin(), m_created.end(), tag) == m_created.end()) {
			throw std::runtime_error(std::format("'{}' is already in memory: another import of a model with this name has not finished", tag.view()));
		}
		return true;
	}

	// Always overwrites (a re-import refreshes geometry and prefab), but only a tag that was not there before is the
	// import's own to erase on a cancel; one that was keeps its new data.
	void store(const res::tag& tag, const std::vector<std::byte>& data) override
	{
		const bool existed = exists(tag);
		m_res.store(tag, data);
		if (existed) {
			egLOG_WARN("edt/import", "'{}' was already there and is kept if the import is cancelled", tag.view());
		} else {
			m_created.push_back(tag);
		}
	}

private:
	res::resource_system& m_res;
	std::vector<res::tag>& m_created;
	edt::model_file_access* m_files;
};

// Erases the memory:// tags a failed or cancelled import has stored so far.
class stored_tags_guard
{
public:
	stored_tags_guard(res::resource_system& res, const std::vector<res::tag>& tags) : m_res(res), m_tags(tags) {}
	~stored_tags_guard()
	{
		if (!m_armed) return;
		for (const auto& tag : m_tags) {
			m_res.erase(tag);
		}
	}
	stored_tags_guard(const stored_tags_guard&) = delete;
	stored_tags_guard& operator=(const stored_tags_guard&) = delete;

	void disarm() { m_armed = false; }

private:
	res::resource_system& m_res;
	const std::vector<res::tag>& m_tags;
	bool m_armed = true;
};

} // anonymous namespace

edt::model_importer::model_importer(desc::desc_system& ds, res::resource_system& rs)
	: m_desc(ds)
	, m_res(rs)
{
}

edt::model_file_scan edt::model_importer::scan_files(const import_source& source, ds::task_context* ctx)
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

edt::scan_requests edt::model_importer::list_requests(const import_source& source, ds::task_context* ctx)
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

edt::import_intermediate edt::model_importer::import_background(const import_source& source, ds::task_context* ctx)
{
	const std::filesystem::path& abs_path = source.path;
	import_intermediate im;
	im.source_path = abs_path;
	std::vector<res::tag> created_tags;
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
	stored_tags_guard cleanup(m_res, created_tags);
	tracking_sink sink(m_res, created_tags, files_access);
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
	}
	checkpoint(ctx);

	// The prefab, stamped by this desc system. The geometry's binary file is not in all_tags: whoever copies the desc
	// carries it along (asset_exporter).
	boost::json::object jsdata = std::move(imported.prefab);
	m_desc.stamp_tree(jsdata);
	im.prefab_tag = options.prefab_tag;
	sink.store(im.prefab_tag, m_desc.serialize_to_bytes(jsdata));
	im.prefab_json = std::move(jsdata);
	im.all_tags = std::move(imported.tags);
	im.all_tags.push_back(im.prefab_tag);
	{
		std::unordered_set<res::tag> seen;
		std::erase_if(im.all_tags, [&seen](const res::tag& t) { return !seen.insert(t).second; });
	}
	im.skin_weights = std::move(imported.skin_weights);
	im.missing_textures = std::move(imported.missing_textures);

	egLOG("edt/import", "Background import done: '{}' → {} memory resources", display_name_of(abs_path), im.all_tags.size());
	for (const auto& t : im.all_tags) {
		egLOG("edt/import", "  tag: {}", t.view());
	}
	im.created_tags = created_tags;
	cleanup.disarm();
	return im;
}

edt::import_result edt::model_importer::finalize_on_main(import_intermediate&& im)
{
	import_result result;
	result.source_path = std::move(im.source_path);
	result.root_tag = im.prefab_tag;
	result.all_tags = std::move(im.all_tags);

	// desc_sys.create_instance + deserialize — main thread only
	auto instance = m_desc.create_instance("prefab_desc", im.prefab_tag);
	if (instance) {
		instance->deserialize(m_desc, im.prefab_json);
		result.prefab = ds::polymorphic_pointer_cast<scn::prefab_desc>(instance);
	}

	return result;
}
