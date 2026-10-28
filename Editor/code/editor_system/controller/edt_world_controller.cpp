#include "edt_world_controller.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_editor_services.h>
#include "edt_editor_camera.h"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <entt/entt.hpp>
#include <boost/json.hpp>
#include <snk/common/logger/engine_log.h>

edt::world_controller::world_controller(scene_document& state, editor_services& services)
	: m_state(state)
	, m_services(services)
{
}

bool edt::world_controller::switch_to_world(int idx)
{
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || idx < 0 || idx >= (int)m_state.world_names.size())
		return false;

	scn::context_set& lvl = ctx_mgr->level_set();

	// The world we leave (or the target) may be gone: a level reload from the desc drops worlds
	// created at runtime while world_names still lists them. Never assert on a stale name.
	scn::world_context* target = lvl.find(m_state.world_names[idx]);
	if (!target) {
		egLOG_ERROR("Editor/SwitchWorld", "World '{}' is not in the level, switch ignored", m_state.world_names[idx]);
		return false;
	}

	scn::world_context* old_world = nullptr;
	if (m_state.active_world_idx >= 0 && m_state.active_world_idx < (int)m_state.world_names.size()) {
		old_world = lvl.find(m_state.world_names[m_state.active_world_idx]);
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
	m_state.touch_descs();
	scn::world_context& world = *target;

	if (on_inject_camera) on_inject_camera(world.state());
	if (on_world_switched) on_world_switched(idx);
	return true;
}

void edt::world_controller::select_world(int idx)
{
	if (idx < 0) {
		clear_active_world();
		return;
	}
	if (switch_to_world(idx)) return;
	// Refused: the ECS and the viewport stay on the old world, so the index must too (or none if that one is gone).
	if (m_state.active_world_idx < 0 || m_state.active_world_idx >= (int)m_state.world_names.size())
		m_state.active_world_idx = -1;
	m_state.touch_descs();
	if (on_world_switched) on_world_switched(m_state.active_world_idx);
}

void edt::world_controller::clear_active_world()
{
	m_state.active_world_idx = -1;
	m_state.touch_descs();
	if (on_world_switched) on_world_switched(-1);
}

void edt::world_controller::create_world(const std::string& name)
{
	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || !m_services.sfactory() || !m_services.assembler()) return;

	if (ctx_mgr->level_set().has(name)) {
		egLOG_WARN("Editor/CreateWorld", "World '{}' already exists, choose another name", name);
		return;
	}

	namespace json = boost::json;

	auto tag = res::tag("edt://templates/world.desc");
	auto text = m_services.resources()->require_sync<res::text_resource>(tag);
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
	m_services.writable_resources()->store(mem_tag, m_services.descs()->serialize_to_bytes(json::value{ world_json }));
	m_services.resources()->signal_changed(mem_tag);

	auto world_res = m_services.resources()->require_sync<scn::world_desc>(mem_tag);
	if (!world_res.is_ready()) {
		egLOG("Editor/CreateWorld", "Failed to create world desc for '{}'", name);
		return;
	}

	scn::context_set& lvl = ctx_mgr->level_set();
	uint32_t new_id = lvl.acquire_free_id();
	scn::world_context& world = lvl.create_context(name, new_id);

	lvl.add_systems(world, world_res->get_systems(), *m_services.sfactory());

	entt::entity e = world.state().create();
	m_services.assembler()->spawn_from_desc(world.state(), e, *world_res, name);

	m_state.world_names.push_back(name);
	m_state.world_descs.push_back(*world_res);
	m_state.worlds_systems_list.push_back(world_res->get_systems());
	m_state.commit("Create world", commit_mode::state_only);
	select_world((int)m_state.world_names.size() - 1);
}
