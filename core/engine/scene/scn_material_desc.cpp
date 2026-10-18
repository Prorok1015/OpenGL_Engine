#include "scn_material_desc.h"
#include "res_system.h"
#include "res_tag_json.h"
#include "resources/res_resource_text.h"
#include "scn_glm_json_convert.h"

constexpr auto SAMPLERS_FIELD = "samplers";
constexpr auto SHADER_VERTEX_FIELD = "shader_vertex";
constexpr auto SHADER_FRAGMENT_FIELD = "shader_fragment";
constexpr auto SHADER_GEOMETRY_FIELD = "shader_geometry";
constexpr auto DEFINES_FIELD = "defines";
constexpr auto QUEUE_FIELD = "queue";
constexpr auto UNIFORMS_FIELD = "uniforms";

namespace
{
	// Scene-side replica of rnd::render_mode_from_string / rnd::to_string (rnd_state_helper). Kept
	// here so the .cpp does not pull rnd_state_helper.h — the string table MUST stay byte-for-byte
	// identical to the render-side table to preserve .desc round-trip (EPIC-49 US-49-5).
	scn::material_render_mode render_mode_from_string(const std::string& str)
	{
		using rm = scn::material_render_mode;
		if (str == "TRIANGLE") return rm::TRIANGLE;
		if (str == "TRIANGLE_STRIP") return rm::TRIANGLE_STRIP;
		if (str == "TRIANGLE_FAN") return rm::TRIANGLE_FAN;
		if (str == "TRIANGLE_ADJ") return rm::TRIANGLE_ADJ;
		if (str == "TRIANGLE_STRIP_ADJ") return rm::TRIANGLE_STRIP_ADJ;
		if (str == "LINE") return rm::LINE;
		if (str == "LINE_STRIP") return rm::LINE_STRIP;
		if (str == "LINE_LOOP") return rm::LINE_LOOP;
		if (str == "LINE_ADJ") return rm::LINE_ADJ;
		if (str == "LINE_STRIP_ADJ") return rm::LINE_STRIP_ADJ;
		if (str == "POINT") return rm::POINT;
		return rm::TRIANGLE; // Default
	}

	std::string render_mode_to_string(scn::material_render_mode mode)
	{
		using rm = scn::material_render_mode;
		switch (mode) {
		case rm::TRIANGLE: return "TRIANGLE";
		case rm::TRIANGLE_STRIP: return "TRIANGLE_STRIP";
		case rm::TRIANGLE_FAN: return "TRIANGLE_FAN";
		case rm::TRIANGLE_ADJ: return "TRIANGLE_ADJ";
		case rm::TRIANGLE_STRIP_ADJ: return "TRIANGLE_STRIP_ADJ";
		case rm::LINE: return "LINE";
		case rm::LINE_STRIP: return "LINE_STRIP";
		case rm::LINE_LOOP: return "LINE_LOOP";
		case rm::LINE_ADJ: return "LINE_ADJ";
		case rm::LINE_STRIP_ADJ: return "LINE_STRIP_ADJ";
		case rm::POINT: return "POINT";
		}

		return "UNKNOWN";
	}
}

void scn::tag_invoke(json::value_from_tag, json::value& out, const scn::pass_queue& c)
{
	switch (c)
	{
	case scn::pass_queue::OPAQUE:
		out = json::value("opaque");
		break;
	case scn::pass_queue::TRANSPARENT:
		out = json::value("transparent");
		break;
	case scn::pass_queue::MIX:
		out = json::value("mix");
		break;
	}
}

scn::pass_queue scn::tag_invoke(json::value_to_tag<scn::pass_queue>, const json::value& obj)
{
	if (obj.is_string()) {
		auto str = obj.get_string();
		if (str == "opaque") {
			return scn::pass_queue::OPAQUE;
		}
		else if (str == "transparent") {
			return scn::pass_queue::TRANSPARENT;
		}
		else if (str == "mix") {
			return scn::pass_queue::MIX;
		}
	}

	return scn::pass_queue::NONE;
}


void scn::material_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains(SAMPLERS_FIELD)) {
		samplers_textures = json::value_to<std::vector<res::tag>>(data.at(SAMPLERS_FIELD));
	}

	if (data.contains(SHADER_GEOMETRY_FIELD)) {
		geometry_shader = json::value_to<res::tag>(data.at(SHADER_GEOMETRY_FIELD));
	}

	if (data.contains(SHADER_VERTEX_FIELD)) {
		vertex_shader = json::value_to<res::tag>(data.at(SHADER_VERTEX_FIELD));
	}

	if (data.contains(SHADER_FRAGMENT_FIELD)) {
		fragment_shader = json::value_to<res::tag>(data.at(SHADER_FRAGMENT_FIELD));
	}

	if (data.contains(DEFINES_FIELD)) {
		defines = json::value_to<std::vector<std::string>>(data.at(DEFINES_FIELD));
	}

	if (data.contains(QUEUE_FIELD)) {
		queue = json::value_to<scn::pass_queue>(data.at(QUEUE_FIELD));
	}

	if (data.contains(UNIFORMS_FIELD)) {
		auto uniforms = data.at(UNIFORMS_FIELD).get_object();
		if (uniforms.contains("albedo")) {
			albedo = json::value_to<glm::vec4>(uniforms.at("albedo"));
		}
		if (uniforms.contains("emissive")) {
			emissive = json::value_to<glm::vec4>(uniforms.at("emissive"));
		}
		if (uniforms.contains("shininess")) {
			shininess = json::value_to<float>(uniforms.at("shininess"));
		}
	}

	if (data.contains("render_mode")) {
		render_mode = render_mode_from_string(json::value_to<std::string>(data.at("render_mode")));
	}
}

void scn::material_desc::serialize(json::object& data) const
{
	/*
		{
			"name": "material",
			"shader_vertex": "shd://vertex_shader.vert",
			"shader_fragment": "shd://fragment_shader.frag",
			"shader_geometry": "shd://geometry_shader.geom",
			"queue" : "qpaque",
			"constants": {
				"LIGHTS_ENABLED": "true"
			},
			"defines": {
				"USE_ANIMATION",
				"USE_NORMAL_MAP",
				"USE_SPECULAR_MAP",
				"USE_TXM_AS_DIFFUSE"
			},

			"samplers": {
				"tex0": "res://txm1.desc",
				"tex1": "res://txm2.desc",
				"tex2": "res://txm3.desc",
				"tex3": "memory://txm4.desc"
			},

			"info": {
				"umiforms": {
					"uWorldModel": "mat4",
					"uWorldMeshMatr": "mat4",
					"diffuseColor": "vec4",
					"emissiveColor": "vec4",
					"shininess": "float"
				},
				"properties": {
					"position": "vec3",
					"uv": "vec2",
					"normal": "vec3"
				},
			},
		}
	*/
	data["__type"] = "material_desc";
	data[DEFINES_FIELD] = json::value_from(defines);
	json::array jssamplers;
	for (auto& sampler : samplers_textures)
	{
		jssamplers.push_back(json::value_from(sampler));
	}
	data[SAMPLERS_FIELD] = jssamplers;
	data[SHADER_VERTEX_FIELD] = json::value_from(vertex_shader);
	data[SHADER_FRAGMENT_FIELD] = json::value_from(fragment_shader);
	data[SHADER_GEOMETRY_FIELD] = json::value_from(geometry_shader);
	data[QUEUE_FIELD] = json::value_from(queue);

	json::object uniforms;
	uniforms["albedo"] = json::value_from(albedo);
	uniforms["emissive"] = json::value_from(emissive);
	data[UNIFORMS_FIELD] = uniforms;
	data["render_mode"] = render_mode_to_string(render_mode);
}
