#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
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
	
	if (!reg.all_of<scn::local_transform>(e))
		reg.emplace<scn::local_transform>(e);
}

namespace {
	// The distance mouse_controller_component had by default: v1 put the camera this far behind camera_desc.transform.
	constexpr float legacy_pivot_distance = 2.f;

	glm::vec3 vec3_of(const json::object* object, std::string_view key)
	{
		const json::value* value = object ? object->if_contains(key) : nullptr;
		if (!value || !value->is_array() || value->as_array().size() != 3) return glm::vec3{ 0.f };
		glm::vec3 out{ 0.f };
		for (std::size_t i = 0; i < 3; ++i) {
			const json::value& item = value->as_array()[i];
			if (item.is_double()) out[static_cast<glm::length_t>(i)] = static_cast<float>(item.as_double());
			else if (item.is_int64()) out[static_cast<glm::length_t>(i)] = static_cast<float>(item.as_int64());
			else if (item.is_uint64()) out[static_cast<glm::length_t>(i)] = static_cast<float>(item.as_uint64());
		}
		return out;
	}

	json::array array_of(glm::vec3 value)
	{
		json::array out;
		for (glm::length_t i = 0; i < 3; ++i) {
			double rounded = std::round(static_cast<double>(value[i]) * 1e5) / 1e5;
			if (rounded == 0.0) rounded = 0.0;
			out.push_back(rounded);
		}
		return out;
	}

	int version_of(const json::object& object)
	{
		const json::value* version = object.if_contains("__version");
		if (!version) return 1;
		if (version->is_int64()) return static_cast<int>(version->as_int64());
		if (version->is_uint64()) return static_cast<int>(version->as_uint64());
		return 1;
	}
}

void scn::migrate_camera_transform_into_node(json::object& node)
{
	const auto components = node.find("components");
	if (components == node.end() || !components->value().is_object()) return;

	std::optional<json::object> placed;
	for (auto& member : components->value().as_object()) {
		if (!member.value().is_object()) continue;
		json::object& camera = member.value().as_object();
		const json::value* type = camera.if_contains("__type");
		if (!type || !type->is_string() || type->as_string() != camera_desc::__type) continue;
		if (version_of(camera) >= 2) continue;
		if (camera.contains("__parent")) {
			// The pose may sit in the desc this one derives from, which is not loaded here: it cannot be baked.
			egLOG_WARN("scene/camera", "a camera that derives from another desc keeps no pose of its own any more: the transform of its node "
				"is the pose, any camera_desc.transform it or its parent has is dropped");
			camera.erase("transform");
			camera["__version"] = 2;
			continue;
		}

		const json::value* transform = camera.if_contains("transform");
		const json::object* legacy = transform && transform->is_object() ? &transform->as_object() : nullptr;
		const glm::vec3 pivot = vec3_of(legacy, "position");
		const glm::vec3 rotation = glm::radians(vec3_of(legacy, "rotation"));
		const glm::mat4 pose = glm::translate(glm::mat4(1.f), pivot) * glm::toMat4(glm::quat(rotation))
			* glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, legacy_pivot_distance));

		// A node rebuilds its rotation as yawPitchRoll(y, x, z), so decompose in the same order.
		float yaw = 0.f, pitch = 0.f, roll = 0.f;
		glm::extractEulerAngleYXZ(pose, yaw, pitch, roll);
		json::object transform_of_node;
		transform_of_node["position"] = array_of(glm::vec3(pose[3]));
		transform_of_node["rotation"] = array_of(glm::degrees(glm::vec3(pitch, yaw, roll)));
		placed = std::move(transform_of_node);

		camera.erase("transform");
		camera["__version"] = 2;
	}
	if (placed) node["transform"] = std::move(*placed);
}

void scn::migrate_camera_v1(json::object& camera)
{
	if (camera.erase("transform") != 0) {
		egLOG_WARN("scene/camera", "camera_desc.transform is gone: the camera takes the transform of its node. "
			"This camera desc is not part of a node, so its pose was dropped");
	}
}