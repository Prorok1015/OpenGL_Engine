#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <snk/common/logger/engine_log.h>

void scn::camera_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	// The schema reads everything the hand-written reader read, but three forms it treats differently. They are settled
	// here, on a copy of the object, before the generic read.
	json::object rest = data;

	// A render-target tag without a protocol lives in the memory:// store (the color_targets and the legacy "texture" are
	// normalized by their codec).
	for (const std::string_view key : { "depth_target", "tp_accum_target", "tp_reveal_target" }) {
		const auto* v = rest.if_contains(key);
		if (v && v->is_string() && v->as_string().find("://") == std::string_view::npos) {
			const std::string tagged = std::string("memory://").append(v->as_string());
			rest[key] = json::value(std::string_view{ tagged });
		}
	}

	// "clear" and "projection" take words only: a number that happens to name an enumerator is not one.
	for (const std::string_view key : { "clear", "projection" }) {
		const auto* v = rest.if_contains(key);
		if (v && !v->is_string())
			rest.erase(key);
	}

	// Render layers (W4, #272). A value that is not an integer is ignored with a warning like an unknown word (the generic
	// reader would take an integral double); an integer outside the range of the inspector (+-16) is clamped to it
	// (the generic reader would keep the current value).
	const json::value* layer = data.if_contains("layer");
	rest.erase("layer");

	desc::schema_desc<camera_desc, desc::schema_kind::component>::deserialize(desc_system, rest);

	if (layer) {
		constexpr std::int64_t max_layer = 16;
		if (const auto* i = layer->if_int64())
			m_layer = static_cast<int>(std::clamp<std::int64_t>(*i, -max_layer, max_layer));
		else if (layer->is_uint64())
			m_layer = static_cast<int>(max_layer);
		else
			egLOG_WARN("scene/camera", "camera layer must be an integer, got '{}'; keeping {}", json::serialize(*layer), m_layer);
	}
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
	cam_comp.layer = desc.layer();
	cam_comp.clear = desc.clear();
	cam_comp.projection = desc.projection();
	cam_comp.ortho_size = desc.ortho_size();
	reg.emplace_or_replace<scn::camera_component>(e, std::move(cam_comp));
	
	scn::mouse_controller_component controller{ .rotation = desc.rotation(), .position = desc.position() };
	reg.emplace_or_replace<scn::local_transform>(e, glm::translate(controller.position) * glm::toMat4(glm::quat(controller.rotation)) * glm::translate(glm::mat4(1.0), glm::vec3(0, 0, controller.distance)));
	reg.emplace_or_replace<scn::mouse_controller_component>(e, controller);
}