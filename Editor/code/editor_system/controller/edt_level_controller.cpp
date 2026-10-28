#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/desc/desc_system.h>
#include <snk/desc/desc_guid.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <boost/json.hpp>
#include <fstream>
#include <filesystem>
#include <format>
#include <snk/common/logger/engine_log.h>

edt::level_controller::level_controller(
	scene_document& state, editor_services& services, res::resource_system& res, desc::desc_system& desc,
	scene_editor& scene_ed, world_controller& world_ctrl)
	: m_state(state)
	, m_services(services)
	, m_res(res)
	, m_desc(desc)
	, m_scene_editor(scene_ed)
	, m_world_ctrl(world_ctrl)
{
}

edt::level_controller::~level_controller()
{
	if (m_state.editor_tag.is_valid())
		m_res.unwatch(m_state.editor_tag, this);
	if (auto ctx_mgr = m_services.ctx_manager.lock())
		ctx_mgr->set_on_reload(nullptr);
}

// ─── New / Save / Quick-save ─────────────────────────────────────────────────

void edt::level_controller::new_level()
{
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return;

	namespace json = boost::json;

	json::object level_json = load_desc_template("level.desc");
	if (level_json.empty()) {
		egLOG("Editor/NewLevel", "level.desc template missing — aborting");
		return;
	}

	res::tag mem_tag{ "memory://editor/new_level.desc" };

	if (m_state.editor_tag.is_valid())
		m_res.unwatch(m_state.editor_tag, this);
	m_state.editor_tag = mem_tag;

	m_res.store(mem_tag, m_desc.serialize_to_bytes(json::value{ level_json }));
	m_res.signal_changed(mem_tag);

	if (!ctx_mgr->load_level(mem_tag)) {
		egLOG("Editor/NewLevel", "Failed to create new level via desc pipeline");
		return;
	}

	m_res.watch(m_state.editor_tag, this, [this](const res::tag&) {
		if (on_editor_world_reloaded) on_editor_world_reloaded();
	});

	auto level_res = m_res.require_sync<scn::level_desc>(m_state.editor_tag);
	if (!level_res.is_ready()) {
		egLOG_ERROR("Editor/NewLevel", "Level desc '{}' failed to load after creation", m_state.editor_tag.view());
		return;
	}
	populate_worlds_from_level(level_res);
	m_state.level_name = "new_level";

	m_state.level_tag = res::tag{};
	m_state.level_guid.clear();
	// world_names/descs/systems already populated (index-aligned) by populate_worlds_from_level.

	m_state.active_world_idx = m_state.world_names.empty() ? -1 : 0;
	m_state.commit("New level", commit_mode::state_only);
	m_world_ctrl.select_world(m_state.active_world_idx);
}

bool edt::level_controller::save_level_as(const std::string& rel_path)
{
	if (!m_state.editor_tag.is_valid()) {
		egLOG("Editor/Save", "No scene loaded - nothing to save.");
		return false;
	}
	if (!write_level_to_disk(rel_path))
		return false;
	m_state.level_tag = res::tag::make(rel_path);
	return true;
}

bool edt::level_controller::quick_save_level()
{
	if (!m_state.level_tag.is_valid())
		return false;

	return write_level_to_disk(std::string(m_state.level_tag.relative()));
}

// ─── Level load finish ──────────────────────────────────────────────────────

void edt::level_controller::finish_level_load(res::res_handle<scn::level_desc>& handle)
{
	// Cleared first so a failed load cannot leave the previous level's guid to be written into the new file.
	m_state.level_guid.clear();

	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return;

	if (ctx_mgr->load_level(m_state.editor_tag)) {
		m_res.watch(m_state.editor_tag, this, [this](const res::tag&) {
			if (on_editor_world_reloaded) on_editor_world_reloaded();
		});

		auto level_res = m_res.require_sync<scn::level_desc>(m_state.editor_tag);
		if (!level_res.is_ready()) {
			egLOG_ERROR("Editor/LoadLevel", "Level desc '{}' failed to load", m_state.editor_tag.view());
			return;
		}
		m_state.level_guid = level_res->get_guid();
		populate_worlds_from_level(level_res);
		m_state.level_name = std::string(m_state.level_tag.relative());
		if (auto p = m_state.level_name.rfind('.'); p != std::string::npos) m_state.level_name.resize(p);
		if (auto p = m_state.level_name.rfind('/');  p != std::string::npos) m_state.level_name = m_state.level_name.substr(p + 1);

		// world_names/descs/systems already populated (index-aligned) by populate_worlds_from_level.
		m_state.mark_clean();
		if (m_state.world_names.empty()) {
			egLOG_WARN("Editor/LoadLevel", "Level '{}' has no worlds", m_state.level_name);
			m_world_ctrl.clear_active_world();
		} else {
			m_state.active_world_idx = 0;
			m_world_ctrl.select_world(0);
		}
	}
}

// ─── Desc-to-scene (asset drop / export auto-add) ──────────────────────────

void edt::level_controller::add_desc_to_scene(const res::res_handle<desc::desc_base>& handle, const std::string& key)
{
	if (!m_state.editor_tag.is_valid() || !handle.is_ready()) return;

	scn::prefab_desc::prefab_comp_node comp;
	comp.parent_desc = handle;

	scn::prefab_desc::prefab_node node;
	node.name = key;
	node.components[key] = std::move(comp);

	m_scene_editor.create_entity_from_node(std::move(node));
	egLOG("Editor/Drop", "Added '{}' to scene", key);
}

// ─── Template & serialization helpers ───────────────────────────────────────

boost::json::object edt::level_controller::load_desc_template(const std::string& filename)
{
	namespace json = boost::json;
	auto tag = res::tag::make("templates/" + filename);
	auto text = m_res.require_sync<res::text_resource>(tag);
	if (text.is_ready()) {
		try {
			json::parse_options opts;
			opts.max_depth = 128;
			return json::parse(text->str(), {}, opts).as_object();
		} catch (...) {
			egLOG("Editor/Template", "Failed to parse template '{}'", filename);
		}
	}
	egLOG("Editor/Template", "Template '{}' not found", filename);
	return {};
}

boost::json::object edt::level_controller::build_level_json() const
{
	const std::string name = m_state.level_name.empty() ? "level" : m_state.level_name;
	return scn::level_desc::build_json(name, m_state.world_descs, m_desc, m_state.level_guid);
}

bool edt::level_controller::write_level_to_disk(const std::string& rel_path)
{
	m_scene_editor.sync_ecs_transforms_to_desc();

	std::filesystem::path frel(rel_path);
	const std::string stem = frel.stem().string();
	const std::string level_name = stem.empty() ? m_state.level_name : stem;

	// Saving the same file keeps its guid; saving to another path (Save As) writes a new file, which gets a new guid.
	const bool same_file = m_state.level_tag.is_valid() && rel_path == m_state.level_tag.relative();
	const std::string guid = (same_file && !m_state.level_guid.empty()) ? m_state.level_guid : desc::generate_guid();

	const auto level_json = scn::level_desc::build_json(level_name, m_state.world_descs, m_desc, guid);

	std::filesystem::path abs_path = m_res.get_resources_path() / frel;
	std::error_code ec;
	std::filesystem::create_directories(abs_path.parent_path(), ec);
	std::ofstream out(abs_path, std::ios::binary);
	if (!out) {
		egLOG("Editor/Save", "Failed to open file for writing: {}", abs_path.string());
		return false;
	}

	// Same writer as every other desc save, so level files are indented and diff well.
	const auto bytes = m_desc.serialize_to_bytes(boost::json::value{ level_json });
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	m_state.level_guid = guid;
	m_state.level_name = level_name;
	m_state.mark_clean();
	egLOG("Editor/Save", "Level saved to {}", abs_path.string());
	return true;
}

void edt::level_controller::resync_worlds_after_reload()
{
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return;
	scn::context_set& lvl = ctx_mgr->level_set();

	auto level_res = m_res.require_sync<scn::level_desc>(lvl.tag());
	if (!level_res.is_ready()) {
		egLOG_WARN("Editor/Reload", "Level desc '{}' not ready after reload, world arrays not resynced", lvl.tag().view());
		return;
	}

	std::vector<scn::world_desc> desc_worlds;
	for (const auto& wh : level_res->get_worlds()) {
		const auto world = wh.get_sync();
		if (!ENGINE_VERIFY(world, "level '{}': world desc failed to load, skipping", level_res.get_tag().view())) {
			continue;
		}
		desc_worlds.push_back(*world);
	}

	apply_world_resync(m_state, m_world_ctrl, desc_worlds, !level_res->get_worlds().empty(),
		[&lvl](const scn::world_desc& w) { return lvl.has(w.get_name()); });
}

void edt::apply_world_resync(
	scene_document& state,
	world_controller& worlds,
	const std::vector<scn::world_desc>& desc_worlds,
	bool desc_lists_worlds,
	const std::function<bool(const scn::world_desc&)>& is_alive)
{
	// Worlds the desc lists but that failed to load are skipped by the caller; those must not wipe the editor's worlds.
	if (desc_worlds.empty() && desc_lists_worlds) {
		egLOG_WARN("Editor/Reload", "No world of the level loaded after reload, world arrays not resynced");
		return;
	}
	state.touch_descs();

	std::string active_name;
	if (const std::string* active = state.active_world_name()) active_name = *active;

	// An empty list is a valid state (idx -1): the arrays are cleared so nothing can index a world that is gone.
	const int idx = resync_world_arrays(desc_worlds, state.world_descs, state.world_names, state.worlds_systems_list, active_name, is_alive);
	if (idx < 0) {
		if (!active_name.empty()) {
			egLOG_WARN("Editor/Reload", "Active world '{}' is gone after reload, the level has no worlds", active_name);
		}
		worlds.clear_active_world();
		return;
	}
	if (!active_name.empty() && state.world_names[idx] != active_name) {
		egLOG_WARN("Editor/Reload", "Active world '{}' is gone after reload, switching to '{}'", active_name, state.world_names[idx]);
	}
	state.active_world_idx = idx;
	// The world list was replaced, so every pointer into it is stale: the panels are told even if the switch is refused.
	worlds.select_world(idx); // the active world may have been rebuilt without the editor camera
}

void edt::level_controller::populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res)
{
	m_state.touch_descs();
	m_state.world_descs.clear();
	m_state.world_names.clear();
	m_state.worlds_systems_list.clear();
	if (!level_res.is_ready()) return;
	// world_descs is the single ordered source of truth (desc order); names and systems
	// are derived from it so all three parallel arrays stay index-aligned.
	for (const auto& wh : level_res->get_worlds()) {
		const auto world = wh.get_sync();
		if (!ENGINE_VERIFY(world, "level '{}': world desc failed to load, skipping", level_res.get_tag().view())) {
			continue;
		}
		m_state.world_descs.push_back(*world);
	}
	derive_world_arrays(m_state.world_descs, m_state.world_names, m_state.worlds_systems_list);
}
