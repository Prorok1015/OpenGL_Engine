#include "edt_world_controller.h"
#include "edt_shared_state.h"
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

	if (!m_state.world_names.empty() && m_state.active_world_idx < (int)m_state.world_names.size()) {
		if (on_save_camera) {
			on_save_camera(lvl_mgr->get_level().get_world(m_state.world_names[m_state.active_world_idx]).state());
		}
	}

	m_state.active_world_idx = idx;
	scn::world& world = lvl_mgr->get_level().get_world(m_state.world_names[idx]);

	if (on_inject_camera) on_inject_camera(world.state());
	if (on_world_switched) on_world_switched(idx);
}

void edt::world_controller::create_world(const std::string& name)
{
	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || !m_state.sfactory || !m_state.assembler) return;

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
	uint32_t new_id = (uint32_t)lvl.get_world_count();
	scn::world& world = lvl.create_world(name, new_id);

	for (const auto& sys : world_res->get_systems())
		m_state.sfactory->create_system(sys, world.state(), lvl.organizer());

	entt::entity e = world.state().create();
	m_state.assembler->spawn_from_desc(world.state(), e, *world_res, name);

	lvl.mark_systems_graphs_dirty();

	m_state.world_names.push_back(name);
	m_state.world_descs.push_back(*world_res);
	m_state.worlds_systems_list.push_back(world_res->get_systems());
	m_state.is_dirty = true;
	switch_to_world((int)m_state.world_names.size() - 1);
}
