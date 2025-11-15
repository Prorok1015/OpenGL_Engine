#pragma once
#include "common.h"
#include "ecs_entity.h"
#include "res_tag.h"
#include <boost/json.hpp>

namespace json = boost::json;

namespace scn
{
	struct material_link_component
	{
		ecs::entity material;
	};

	struct base_material_component {
		ds::color albedo = ds::color(1.0f);
		ds::color specular = ds::color(1.0f);
		ds::color ambient = ds::color(1.0f);
		ds::color emissive = ds::color(0.0f);
		float shininess = 0.0f; // 0.0 -> 1000
	};

	struct transparent_material_component {
		ds::color transparent = ds::color(1.0f);
		float opacity = 1.0f;
	};

	struct reflective_material_component {
		ds::color reflective = ds::color(1.0f);
		float reflectivity = 0.0f; // 0.0 -> 1.0
	};

	struct refractive_material_component {
		float refracti = 0.0f;
	};

	struct shininess_strength_component {
		float shininess_strength = 0.0f; // 0.0 -> 1.0
	};

	struct is_transparent_flag_component {};
	
	struct albedo_map_component { res::tag txm; };
	struct normal_map_component  { res::tag txm; };
	struct roughness_map_component { res::tag txm; };
	struct metallic_map_component { res::tag txm; };
	struct ao_map_component { res::tag txm; };
	struct height_map_component { res::tag txm; };
	struct specular_map_component { res::tag txm; };
	struct glossiness_map_component { res::tag txm; };
	struct emissive_map_component { res::tag txm; };
	struct opacity_map_component { res::tag txm; };


	void tag_invoke(json::value_from_tag, json::value& jv, const base_material_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const transparent_material_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const reflective_material_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const refractive_material_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const shininess_strength_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const albedo_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const normal_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const roughness_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const metallic_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const ao_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const height_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const specular_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const glossiness_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const emissive_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const opacity_map_component& c);
	void tag_invoke(json::value_from_tag, json::value& jv, const is_transparent_flag_component& c);

	json::value convert_material_to_json(const ecs::entity& ent);
}