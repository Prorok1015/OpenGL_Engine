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

	// Render targets
	if (auto* arr = data.if_contains("color_targets")) {
		for (const auto& v : arr->as_array()) {
			auto tag_str = v.as_string();
			m_color_targets.push_back(res::tag(std::string(tag_str)));
		}
	} else if (data.contains("texture")) {
		// Backward compatibility: old single "texture" field → color_targets[0]
		auto tag_str = data.at("texture").as_string();
		m_color_targets.push_back(res::tag(std::string(tag_str)));
	}
	if (data.contains("depth_target"))
		m_depth_target = res::tag(std::string(data.at("depth_target").as_string()));
	if (data.contains("tp_accum_target"))
		m_tp_accum_target = res::tag(std::string(data.at("tp_accum_target").as_string()));
	if (data.contains("tp_reveal_target"))
		m_tp_reveal_target = res::tag(std::string(data.at("tp_reveal_target").as_string()));
}

void scn::camera_desc::serialize(json::object& data) const
{
	data["fov"] = m_fov;
	data["near_distance"] = m_near_distance;
	data["far_distance"] = m_far_distance;

	if (!m_color_targets.empty()) {
		json::array arr;
		for (const auto& t : m_color_targets)
			arr.push_back(json::string(t.string()));
		data["color_targets"] = std::move(arr);
	}
	if (m_depth_target.is_valid())
		data["depth_target"] = json::string(m_depth_target.string());
	if (m_tp_accum_target.is_valid())
		data["tp_accum_target"] = json::string(m_tp_accum_target.string());
	if (m_tp_reveal_target.is_valid())
		data["tp_reveal_target"] = json::string(m_tp_reveal_target.string());
}

void scn::assemble_camera(entt::registry& reg, entt::entity e, const camera_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::renderable>(e);
	scn::camera_component cam_comp;
	cam_comp.fov = desc.fov();
	cam_comp.near_distance = desc.near_distance();
	cam_comp.far_distance = desc.far_distance();
	cam_comp.color_targets = desc.color_targets();
	cam_comp.depth_target = desc.depth_target();
	cam_comp.tp_accum_target = desc.tp_accum_target();
	cam_comp.tp_reveal_target = desc.tp_reveal_target();
	reg.emplace_or_replace<scn::camera_component>(e, std::move(cam_comp));
	
	scn::mouse_controller_component controller{ .rotation = desc.rotation(), .position = desc.position() };
	reg.emplace_or_replace<scn::local_transform>(e, glm::translate(controller.position) * glm::toMat4(glm::quat(controller.rotation)) * glm::translate(glm::mat4(1.0), glm::vec3(0, 0, controller.distance)));
	reg.emplace_or_replace<scn::mouse_controller_component>(e, controller);
}