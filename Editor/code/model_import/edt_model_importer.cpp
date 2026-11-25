#include "edt_model_importer.h"

#include <snk/model_import/mdl_import.h>
#include <snk/common/logger/engine_log.h>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace {

// What the editor's import writes, over the resource system: `all` gets every tag the import refers to, `created` the
// ones this run stored itself because they did not exist yet; only those may be erased again (the others belong to an
// earlier import of the same model). With `agents` (an agent's import) nothing already in memory:// is reused.
class tracking_sink final : public mdl::import_sink
{
public:
	tracking_sink(res::resource_system& res, std::vector<res::tag>& created, bool agents)
		: m_res(res), m_created(created), m_agents(agents)
	{
	}

	// True when the tag was stored by this run before. A tag from anywhere else is not this import's to reuse or
	// overwrite in an agent's import: its bytes could belong to another model.
	bool exists(const res::tag& tag) override
	{
		if (!m_res.exists(tag)) return false;
		if (m_agents && std::find(m_created.begin(), m_created.end(), tag) == m_created.end()) {
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
	// An import with the files it may read (an agent's): it reuses nothing it did not store itself.
	bool m_agents;
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
	return scan_model_files(source, ctx);
}

edt::scan_requests edt::model_importer::list_requests(const import_source& source, ds::task_context* ctx)
{
	return list_model_requests(source, ctx);
}

edt::import_intermediate edt::model_importer::import_background(const import_source& source, ds::task_context* ctx)
{
	std::vector<res::tag> created_tags;
	stored_tags_guard cleanup(m_res, created_tags);
	tracking_sink sink(m_res, created_tags, source.allowed != nullptr || source.provided != nullptr);
	import_intermediate im = import_model_into(source, sink, ctx, [this](boost::json::object& prefab) { m_desc.stamp_tree(prefab); });
	if (!im.error.empty()) {
		return im;
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
	result.settings = im.settings;
	result.source_sha256 = std::move(im.source_sha256);

	// desc_sys.create_instance + deserialize — main thread only
	auto instance = m_desc.create_instance("prefab_desc", im.prefab_tag);
	if (instance) {
		instance->deserialize(m_desc, im.prefab_json);
		result.prefab = ds::polymorphic_pointer_cast<scn::prefab_desc>(instance);
	}

	return result;
}
