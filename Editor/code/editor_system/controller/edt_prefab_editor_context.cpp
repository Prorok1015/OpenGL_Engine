#include "edt_prefab_editor_context.h"
#include "edt_shared_state.h"
#include "res_system.h"
#include "desc_system.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "level/scn_world.h"
#include "level/scn_world_desc.h"
#include "level/scn_prefab_desc.h"
#include "scn_ecs_assembler.h"
#include "ecs_system.h"
#include "scn_model.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "eng_transform_3d.hpp"
#include "resources/res_resource_text.h"
#include "engine_log.h"
#include <entt/entt.hpp>
#include <glm/gtx/quaternion.hpp>
#include <boost/json.hpp>

edt::prefab_editor_context::prefab_editor_context(shared_state& state, res::resource_system& res, desc::desc_system& desc)
	: m_state(state)
	, m_res(res)
	, m_desc(desc)
{
}

void edt::prefab_editor_context::open(const res::tag& prefab_tag)
{
	if (m_open) {
		close();
	}

	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || !m_state.sfactory || !m_state.assembler) {
		egLOG_ERROR("Editor/PrefabCtx", "Cannot open prefab: services not available");
		return;
	}

	// Load prefab resource
	auto prefab_handle = m_res.require_sync<scn::prefab_desc>(prefab_tag);
	if (!prefab_handle.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to load prefab '{}'", prefab_tag.view());
		return;
	}

	// Load preview template
	auto template_tag = res::tag::make("templates/prefab_preview.desc");
	auto template_text = m_res.require_sync<res::text_resource>(template_tag);
	if (!template_text.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to load prefab_preview.desc template");
		return;
	}

	namespace json = boost::json;
	json::object world_json;
	try {
		json::parse_options opts;
		opts.max_depth = 128;
		world_json = json::parse(template_text->str(), {}, opts).as_object();
	} catch (...) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to parse prefab_preview.desc template");
		return;
	}

	// Store template as memory resource for desc_system to parse
	res::tag mem_tag{ "memory://editor/__prefab_preview.desc" };
	m_res.store(mem_tag, m_desc.serialize_to_bytes(json::value{ world_json }));
	m_res.signal_changed(mem_tag);

	auto world_res = m_res.require_sync<scn::world_desc>(mem_tag);
	if (!world_res.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to create world desc for prefab preview");
		return;
	}

	// Create transient world in level — NOT registered in shared_state
	scn::level& lvl = lvl_mgr->get_level();
	m_world_id = static_cast<uint32_t>(lvl.get_world_count());
	scn::world& world = lvl.create_world(std::string{WORLD_NAME}, m_world_id);

	// Register systems from template
	for (const auto& sys : world_res->get_systems()) {
		m_state.sfactory->create_system(sys, world.state(), lvl.organizer());
	}

	// Assemble template (anchor, light, etc.)
	entt::entity root = world.state().create();
	m_state.assembler->spawn_from_desc(world.state(), root, *world_res, WORLD_NAME);

	// Inject prefab camera with separate render targets
	inject_camera(world.state());

	// Load the prefab into the world
	entt::entity prefab_root = world.state().create();
	scn::assemble_prefab(*m_state.assembler, world.state(), prefab_root, *prefab_handle, prefab_handle->get_root().name, prefab_tag);

	lvl.mark_systems_graphs_dirty();

	m_source_tag = prefab_tag;
	m_prefab_name = std::string(prefab_tag.pure_name());
	m_open = true;
	m_dirty = false;

	egLOG("Editor/PrefabCtx", "Opened prefab '{}' for editing", prefab_tag.view());
}

void edt::prefab_editor_context::close()
{
	if (!m_open) return;

	auto lvl_mgr = m_state.lvl_manager.lock();
	if (lvl_mgr) {
		scn::level& lvl = lvl_mgr->get_level();
		if (lvl.has_world(WORLD_NAME)) {
			lvl.remove_world(WORLD_NAME);

			// Rebuild organizer for remaining (scene) worlds
			lvl.clear_organizer();
			for (size_t i = 0; i < m_state.world_names.size(); ++i) {
				if (!lvl.has_world(m_state.world_names[i])) continue;
				auto& world = lvl.get_world(m_state.world_names[i]);
				for (const auto& sys_name : m_state.worlds_systems_list[i]) {
					m_state.sfactory->create_system(sys_name, world.state(), lvl.organizer());
				}
			}
			lvl.mark_systems_graphs_dirty();
		}
	}

	m_open = false;
	m_dirty = false;
	m_world_id = UINT32_MAX;
	m_source_tag = res::tag::null;
	m_prefab_name.clear();

	egLOG("Editor/PrefabCtx", "Closed prefab editor");
}

entt::registry* edt::prefab_editor_context::get_registry()
{
	if (!m_open) return nullptr;

	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr) return nullptr;

	scn::level& lvl = lvl_mgr->get_level();
	if (!lvl.has_world(WORLD_NAME)) return nullptr;

	return &lvl.get_world(WORLD_NAME).state();
}

void edt::prefab_editor_context::inject_camera(entt::registry& reg)
{
	entt::entity cam = reg.create();
	reg.emplace<edt::prefab_camera_tag>(cam);

	scn::camera_component cam_comp;
	cam_comp.fov           = m_camera_state.fov;
	cam_comp.near_distance = m_camera_state.near_distance;
	cam_comp.far_distance  = m_camera_state.far_distance;
	cam_comp.color_targets.push_back(res::tag{ RT_COLOR });
	cam_comp.depth_target      = res::tag{ RT_DEPTH };
	cam_comp.tp_accum_target   = res::tag{ RT_TP_ACCUM };
	cam_comp.tp_reveal_target  = res::tag{ RT_TP_REVEAL };
	reg.emplace<scn::camera_component>(cam, cam_comp);

	scn::mouse_controller_component ctrl;
	ctrl.position       = m_camera_state.position;
	ctrl.rotation       = m_camera_state.rotation;
	ctrl.distance       = m_camera_state.distance;
	ctrl.movement_speed = m_camera_state.movement_speed;
	ctrl.rotating_speed = m_camera_state.rotating_speed;
	reg.emplace<scn::mouse_controller_component>(cam, ctrl);

	glm::mat4 orientation = glm::toMat4(glm::quat(ctrl.rotation));
	glm::mat4 cam_matrix = glm::translate(glm::mat4(1.0f), ctrl.position)
		* orientation
		* glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, ctrl.distance));
	reg.emplace<scn::local_transform>(cam, scn::local_transform{ cam_matrix });
	reg.emplace<scn::world_transform>(cam);
	reg.emplace<scn::name_component>(cam, "Prefab Camera");
	reg.emplace<scn::depth_level>(cam);
}
