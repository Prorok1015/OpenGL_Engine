#include "scn_camera_desc.h"
#include "scn_glm_json_convert.h"
#include "scn_model.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"

void scn::camera_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("fov"))
		m_fov = data.at("fov").as_double();
	if (data.contains("near_distance"))
		m_near_distance = data.at("near_distance").as_double();
	if (data.contains("far_distance"))
		m_far_distance = data.at("far_distance").as_double();
	if (data.contains("position"))
		m_position = json::value_to<glm::vec3>(data.at("position"));
	if (data.contains("rotation"))
		m_rotation = json::value_to<glm::vec3>(data.at("rotation"));
	if (data.contains("scale"))
		m_scale = json::value_to<glm::vec3>(data.at("scale"));
}

void scn::camera_desc::serialize(json::object& data) const
{
	data["fov"] = m_fov;
	data["near_distance"] = m_near_distance;
	data["far_distance"] = m_far_distance;
	data["position"] = json::value_from(m_position);
	data["rotation"] = json::value_from(m_rotation);
	data["scale"] = json::value_from(m_scale);
}

void scn::assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name)
{
	reg.emplace<scn::name_component>(e, std::string{ name });
	reg.emplace<scn::local_transform>(e, desc.get_transform());
	reg.emplace<scn::world_transform>(e);
	reg.emplace<scn::renderable>(e);
	reg.emplace<scn::camera_component>(e, scn::camera_component{ .fov = desc.fov(), .near_distance = desc.near_distance(), .far_distance = desc.far_distance()});
	reg.emplace<scn::mouse_controller_component>(e, scn::mouse_controller_component{ .rotation = desc.rotation()});
	if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
		reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit((e));
	} else {
		ecs::event<scn::hierarchy_updated> event;
		event.emit((e));
		reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
	}
	if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
		reg.ctx().get<ecs::event<scn::transform_updated>>().emit((e));
	} else {
		ecs::event<scn::transform_updated> event;
		event.emit(e);
		reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
	}
}