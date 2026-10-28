#include "edt_prefab_document.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <snk/desc/desc_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/common/logger/engine_log.h>
#include <entt/entt.hpp>
#include <glm/gtx/quaternion.hpp>
#include <boost/json.hpp>

edt::prefab_document::prefab_document(editor_services& services)
	: m_services(services)
{
}

void edt::prefab_document::open(const res::tag& prefab_tag)
{
	if (m_open) {
		close();
	}

	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr || !m_services.sfactory() || !m_services.assembler()) {
		egLOG_ERROR("Editor/PrefabCtx", "Cannot open prefab: services not available");
		return;
	}

	// Load prefab resource
	auto prefab_handle = m_services.resources()->require_sync<scn::prefab_desc>(prefab_tag);
	if (!prefab_handle.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to load prefab '{}'", prefab_tag.view());
		return;
	}

	// Load preview template
	auto template_tag = res::tag::make("templates/prefab_preview.desc");
	auto template_text = m_services.resources()->require_sync<res::text_resource>(template_tag);
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
	m_services.writable_resources()->store(mem_tag, m_services.descs()->serialize_to_bytes(json::value{ world_json }));
	m_services.resources()->signal_changed(mem_tag);

	auto world_res = m_services.resources()->require_sync<scn::world_desc>(mem_tag);
	if (!world_res.is_ready()) {
		egLOG_ERROR("Editor/PrefabCtx", "Failed to create world desc for prefab preview");
		return;
	}

	// The preview is a set of its own, so it neither shares the level's time nor its names.
	scn::world_context& preview = ctx_mgr->create_preview(prefab_tag.view(), CONTEXT_NAME, world_res->get_systems());
	m_set_name = scn::context_manager::preview_set_name(prefab_tag.view());
	entt::registry& reg = preview.state();

	// Assemble template (anchor, light, etc.)
	entt::entity root = reg.create();
	m_services.assembler()->spawn_from_desc(reg, root, *world_res, CONTEXT_NAME);

	// Inject prefab camera with separate render targets
	inject_camera(reg);

	// Load the prefab into the world
	entt::entity prefab_root = reg.create();
	scn::assemble_prefab(*m_services.assembler(), reg, prefab_root, *prefab_handle, prefab_handle->get_root().name, prefab_tag);

	m_source_tag = prefab_tag;
	m_prefab_name = std::string(prefab_tag.pure_name());
	m_open = true;
	m_dirty = false;

	egLOG("Editor/PrefabCtx", "Opened prefab '{}' for editing", prefab_tag.view());
}

void edt::prefab_document::close()
{
	if (!m_open) return;

	if (auto ctx_mgr = m_services.ctx_manager.lock()) {
		ctx_mgr->destroy_preview(m_source_tag.view());
	}

	m_open = false;
	m_dirty = false;
	m_set_name.clear();
	m_source_tag = res::tag::null;
	m_prefab_name.clear();

	egLOG("Editor/PrefabCtx", "Closed prefab editor");
}

entt::registry* edt::prefab_document::get_registry()
{
	if (!m_open) return nullptr;

	auto ctx_mgr = m_services.ctx_manager.lock();
	if (!ctx_mgr) return nullptr;

	scn::world_context* preview = ctx_mgr->find_preview(m_source_tag.view());
	return preview ? &preview->state() : nullptr;
}

void edt::prefab_document::inject_camera(entt::registry& reg)
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
	// The camera has no parent and nothing emits transform_updated for it until it moves, so the
	// transform system would leave an identity world matrix (and the render camera at the origin).
	reg.emplace<scn::world_transform>(cam, scn::world_transform{ cam_matrix });
	reg.emplace<scn::name_component>(cam, "Prefab Camera");
	reg.emplace<scn::depth_level>(cam);
}
