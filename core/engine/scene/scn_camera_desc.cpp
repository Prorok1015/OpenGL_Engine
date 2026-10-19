#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_glm_json_convert.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <string_view>

namespace {
	// as_double() throws on JSON integers (`"fov": 60`); to_number accepts any numeric kind.
	float read_number(const json::value& v)
	{
		return v.to_number<float>();
	}

	// Runtime render targets live in the memory:// store. res::tag(std::string) slices garbage
	// from a string without a protocol, so add the canonical one when it is missing.
	res::tag read_rt_tag(const json::value& v)
	{
		const std::string_view str = v.as_string();
		if (str.find("://") != std::string_view::npos)
			return res::tag(str);
		return res::tag(res::tag::memory, str);
	}

	// The words of "clear" and "projection" in the desc; an unknown word keeps the value the camera has.
	void read_clear(const json::value& v, scn::camera_clear& out)
	{
		if (!v.is_string()) return;
		const std::string_view word = v.as_string();
		if (word == "all") out = scn::camera_clear::all;
		else if (word == "depth") out = scn::camera_clear::depth;
		else if (word == "none") out = scn::camera_clear::none;
	}

	void read_projection(const json::value& v, scn::camera_projection& out)
	{
		if (!v.is_string()) return;
		const std::string_view word = v.as_string();
		if (word == "perspective") out = scn::camera_projection::perspective;
		else if (word == "ortho") out = scn::camera_projection::ortho;
	}

	std::string_view clear_word(scn::camera_clear clear)
	{
		switch (clear) {
		case scn::camera_clear::depth: return "depth";
		case scn::camera_clear::none: return "none";
		default: return "all";
		}
	}
}

void scn::camera_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("fov"))
		m_fov = read_number(data.at("fov"));
	if (data.contains("near_distance"))
		m_near_distance = read_number(data.at("near_distance"));
	if (data.contains("far_distance"))
		m_far_distance = read_number(data.at("far_distance"));

	if (auto* trans = data.if_contains("transform")) {
		if (trans->as_object().contains("rotation"))
			m_rotation = json::value_to<glm::vec3>(trans->at("rotation"));
		if (trans->as_object().contains("position"))
			m_position = json::value_to<glm::vec3>(trans->at("position"));
	}

	// Render targets
	// Replace (not append) so an override layered on a copy of its parent does not duplicate targets.
	if (auto* arr = data.if_contains("color_targets")) {
		m_color_targets.clear();
		for (const auto& v : arr->as_array()) {
			m_color_targets.push_back(read_rt_tag(v));
		}
	} else if (data.contains("texture")) {
		// Backward compatibility: old single "texture" field → color_targets[0]
		m_color_targets.clear();
		m_color_targets.push_back(read_rt_tag(data.at("texture")));
	}
	if (data.contains("depth_target"))
		m_depth_target = read_rt_tag(data.at("depth_target"));
	if (data.contains("tp_accum_target"))
		m_tp_accum_target = read_rt_tag(data.at("tp_accum_target"));
	if (data.contains("tp_reveal_target"))
		m_tp_reveal_target = read_rt_tag(data.at("tp_reveal_target"));

	// Render layers (W4, #272). A number that is not an integer is ignored like an unknown word.
	if (auto* layer = data.if_contains("layer"); layer && layer->is_number())
		m_layer = static_cast<int>(layer->to_number<std::int64_t>());
	if (auto* clear = data.if_contains("clear"))
		read_clear(*clear, m_clear);
	if (auto* projection = data.if_contains("projection"))
		read_projection(*projection, m_projection);
	if (data.contains("ortho_size"))
		m_ortho_size = read_number(data.at("ortho_size"));
}

void scn::camera_desc::serialize(json::object& data) const
{
	data["fov"] = m_fov;
	data["near_distance"] = m_near_distance;
	data["far_distance"] = m_far_distance;

	json::object transform;
	transform["rotation"] = json::value_from(m_rotation);
	transform["position"] = json::value_from(m_position);
	data["transform"] = std::move(transform);

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

	// The defaults are not written, so cameras saved before the fields existed stay byte-identical.
	if (m_layer != 0)
		data["layer"] = m_layer;
	if (m_clear != camera_clear::all)
		data["clear"] = clear_word(m_clear);
	if (m_projection != camera_projection::perspective)
		data["projection"] = "ortho";
	if (m_ortho_size != default_camera_ortho_size)
		data["ortho_size"] = m_ortho_size;
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