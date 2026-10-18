#include "edt_world_controller.h"
#include "edt_shared_state.h"
#include "edt_editor_camera.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "level/scn_world_desc.h"
#include "ecs_system.h"
#include "scn_ecs_assembler.h"
#include "desc_system.h"
#include "res_system.h"
#include "resources/res_resource_text.h"
#include <entt/entt.hpp>
#include <boost/json.hpp>
#include "engine_log.h"

edt::world_controller::world_controller(shared_state& state, res::resource_system& res, desc::desc_system& desc)
	: m_state(state)
	, m_res(res)
	, m_desc(desc)
{
}

void edt::world_controller::switch_to_world(int idx)
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || idx < 0 || idx >= (int)m_state.world_names.size())
		return;

	scn::level& lvl = lvl_mgr->get_level();

	// The world we leave (or the target) may be gone: a level reload from the desc drops worlds
	// created at runtime while world_names still lists them. Never assert on a stale name.
	scn::world* target = lvl.find_world(m_state.world_names[idx]);
	if (!target) {
		egLOG_ERROR("Editor/SwitchWorld", "World '{}' is not in the level, switch ignored", m_state.world_names[idx]);
		return;
	}

	scn::world* old_world = nullptr;
	if (m_state.active_world_idx >= 0 && m_state.active_world_idx < (int)m_state.world_names.size()) {
		old_world = lvl.find_world(m_state.world_names[m_state.active_world_idx]);
	}

	if (old_world && on_save_camera) {
		on_save_camera(old_world->state());
	}

	// render_data_extractor renders every world's cameras into their targets. A left-over editor camera
	// in the world we leave would keep drawing into the shared editor RT and overdraw the new world.
	if (old_world && idx != m_state.active_world_idx) {
		auto& old_reg = old_world->state();
		for (auto ent : old_reg.view<edt::editor_camera_tag>())
			old_reg.destroy(ent);
	}

	m_state.active_world_idx = idx;
	scn::world& world = *target;

	if (on_inject_camera) on_inject_camera(world.state());
	if (on_world_switched) on_world_switched(idx);
}

void edt::world_controller::clear_active_world()
{
	m_state.active_world_idx = -1;
	if (on_world_switched) on_world_switched(-1);
}

void edt::world_controller::create_world(const std::string& name)
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || !m_state.sfactory || !m_state.assembler) return;

	if (lvl_mgr->get_level().has_world(name)) {
		egLOG_WARN("Editor/CreateWorld", "World '{}' already exists, choose another name", name);
		return;
	}

	namespace json = boost::json;

	auto tag = res::tag::make("templates/world.desc");
	auto text = m_res.require_sync<res::text_resource>(tag);
	json::object world_json;
	if (text.is_ready()) {
		try {
			json::parse_options opts;
			opts.max_depth = 128;
			world_json = json::parse(text->str(), {}, opts).as_object();
		} catch (...) {
			egLOG("Editor/CreateWorld", "Failed to parse world.desc template");
			return;
		}
	} else {
		egLOG("Editor/CreateWorld", "world.desc template not found");
		return;
	}

	world_json["name"] = name;

	res::tag mem_tag{ "memory://editor/world_" + name + ".desc" };
	m_res.store(mem_tag, m_desc.serialize_to_bytes(json::value{ world_json }));
	m_res.signal_changed(mem_tag);

	auto world_res = m_res.require_sync<scn::world_desc>(mem_tag);
	if (!world_res.is_ready()) {
		egLOG("Editor/CreateWorld", "Failed to create world desc for '{}'", name);
		return;
	}

	scn::level& lvl = lvl_mgr->get_level();
	uint32_t new_id = lvl.acquire_free_world_id();
	scn::world& world = lvl.create_world(name, new_id);

	lvl.add_world_systems(world, world_res->get_systems(), *m_state.sfactory);

	entt::entity e = world.state().create();
	m_state.assembler->spawn_from_desc(world.state(), e, *world_res, name);

	m_state.world_names.push_back(name);
	m_state.world_descs.push_back(*world_res);
	m_state.worlds_systems_list.push_back(world_res->get_systems());
	m_state.is_dirty = true;
	switch_to_world((int)m_state.world_names.size() - 1);
}
