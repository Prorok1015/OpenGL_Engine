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

	if (auto* trans = data.if_contains("transform")) {
		if (trans->as_object().contains("rotation"))
			m_rotation = json::value_to<glm::vec3>(trans->at("rotation"));
		if (trans->as_object().contains("position"))
			m_position = json::value_to<glm::vec3>(trans->at("position"));
	}
}

void scn::camera_desc::serialize(json::object& data) const
{
	data["fov"] = m_fov;
	data["near_distance"] = m_near_distance;
	data["far_distance"] = m_far_distance;
}

void scn::assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::renderable>(e);
	reg.emplace_or_replace<scn::camera_component>(e, scn::camera_component{ .fov = desc.fov(), .near_distance = desc.near_distance(), .far_distance = desc.far_distance(), .texture = desc.texture() });
	
	scn::mouse_controller_component controller{ .rotation = desc.rotation(), .position = desc.position() };
	reg.emplace_or_replace<scn::local_transform>(e, glm::translate(controller.position) * glm::toMat4(glm::quat(controller.rotation)) * glm::translate(glm::mat4(1.0), glm::vec3(0, 0, controller.distance)));
	reg.emplace_or_replace<scn::mouse_controller_component>(e, controller);
}