#include "scn_material_desc.h"
#include "res_system.h"
#include "res_resource_text_file.h"
#include "rnd_driver_interface.h"
#include "scn_model.h"

constexpr auto SAMPLERS_FIELD = "samplers";
constexpr auto SHADER_VERTEX_FIELD = "shader_vertex";
constexpr auto SHADER_FRAGMENT_FIELD = "shader_fragment";
constexpr auto SHADER_GEOMETRY_FIELD = "shader_geometry";
constexpr auto DEFINES_FIELD = "defines";
constexpr auto QUEUE_FIELD = "queue";



void scn::tag_invoke(json::value_from_tag, json::value& out, const scn::pass_queue& c)
{
	switch (c)
	{
	case scn::pass_queue::OPAQUE:
		out = json::value("opaque");
		break;
	case scn::pass_queue::TRANSPARENT:
		out = json::value("transparent");
	case scn::pass_queue::MIX:
		out = json::value("mix");
		break;
	default:
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
		//samplers_textures = json::value_to<std::vector<res::tag>>(data.at(SAMPLERS_FIELD));
		for (auto& sampler : data.at(SAMPLERS_FIELD).get_array())
		{
			samplers_textures_desc.push_back(desc_system.get_or_override_desc<rnd::texture_desc>(*this, sampler));
		}
	}

	if (data.contains(SHADER_GEOMETRY_FIELD)) {
		cdata.program.set_geometry_shader(
			json::value_to<res::tag>(data.at(SHADER_GEOMETRY_FIELD)
			));
	}

	if (data.contains(SHADER_VERTEX_FIELD)) {
		cdata.program.set_vertex_shader(
			json::value_to<res::tag>(data.at(SHADER_VERTEX_FIELD)
			));
	}

	if (data.contains(SHADER_FRAGMENT_FIELD)) {
		cdata.program.set_fragment_shader(
			json::value_to<res::tag>(data.at(SHADER_FRAGMENT_FIELD)
			));
	}

	if (data.contains(DEFINES_FIELD)) {
		cdata.defines = json::value_to<std::vector<std::string>>(data.at(DEFINES_FIELD));
	}

	if (data.contains(QUEUE_FIELD)) {
		queue = json::value_to<scn::pass_queue>(data.at(QUEUE_FIELD));
	}
}

void scn::material_desc::serialize(const res::tag& tag, res::resource_system& res_system, json::object& data) const
{
	/*
		{
			"name": "material",
			"shader_vertex": "shd://vertex_shader.vert",
			"shader_fragment": "shd://fragment_shader.frag",
			"shader_geometry": "shd://geometry_shader.geom",
			"queue" : "qpaque",
			"constants": {
				"LIGHTS_ENABLED": "true",
				"DIRECTION_LIGHT_COUNT": "4",
				"POINT_LIGHT_COUNT": "8"
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

	data[DEFINES_FIELD] = json::value_from(cdata.defines);
	json::array jssamplers;
	for (auto& sampler : samplers_textures_desc)
	{
		jssamplers.push_back(json::value_from(sampler->get_tag()));
	}
	data[SAMPLERS_FIELD] = jssamplers;
	data[SHADER_VERTEX_FIELD] = json::value_from(cdata.program.get_vertex_shader());
	data[SHADER_FRAGMENT_FIELD] = json::value_from(cdata.program.get_fragment_shader());
	data[SHADER_GEOMETRY_FIELD] = json::value_from(cdata.program.get_geometry_shader());
	data[QUEUE_FIELD] = json::value_from(queue);
}

rnd::new_shader_desc scn::material_desc::get_shader_desc(entt::handle handle, rnd::TextureManager& txm_manager)
{
	rnd::new_shader_desc::runtime_data rdata;
	rdata.samplers.resize(samplers_textures_desc.size());
	for (int idx = 0; idx < samplers_textures_desc.size(); ++idx)
	{
		if (samplers_textures_desc[idx]) {
			rdata.samplers[idx] = txm_manager.require_texture(samplers_textures_desc[idx]->get_tag());
		} else {
			rdata.samplers[idx] = nullptr;
		}
	}

	if (handle.all_of<scn::world_transform>()) {
		auto& transform = handle.get<scn::world_transform>();
		rdata.uniforms["uWorldMeshMatr"] = transform.world;
	}

	rdata.uniforms["uWorldModel"] = glm::mat4(1.0);
	rdata.uniforms["diffuseColor"] = glm::vec4(1, 0, 0, 1);
	rdata.uniforms["emissiveColor"] = glm::vec4(0);
	rdata.uniforms["shininess"] = 32.f;

	return rnd::new_shader_desc{ cdata, rdata };
}