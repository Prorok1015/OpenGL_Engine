#include "scn_material_component.hpp"
#include "scn_model.h"
#include "ecs_common_system.h"
#include "scn_glm_json_convert.h"

void scn::tag_invoke(json::value_from_tag, json::value& jv, const base_material_component& c )
{
    jv = {
        { "albedo" , json::value_from(c.albedo) },
        { "specular", json::value_from(c.specular) },
        { "ambient", json::value_from(c.ambient) },
        { "emissive", json::value_from(c.emissive) },
        { "shininess", c.shininess }
    };
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const transparent_material_component& c )
{
    jv = {
        { "transparent", json::value_from(c.transparent) },
        { "opacity", c.opacity }
    };
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const reflective_material_component& c )
{
    jv = {
        { "reflective", json::value_from(c.reflective) },
        { "reflectivity", c.reflectivity }
    };
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const refractive_material_component& c )
{
    jv = c.refracti;
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const shininess_strength_component& c )
{
    jv = c.shininess_strength;
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const albedo_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const normal_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const roughness_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const metallic_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const ao_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const height_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const specular_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const glossiness_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const emissive_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const opacity_map_component& c )
{
    jv = c.txm.get_full();
}

void scn::tag_invoke(json::value_from_tag, json::value& jv, const is_transparent_flag_component& c )
{
    jv = true;
}

boost::json::value scn::convert_material_to_json(const ecs::entity& ent)
{
    boost::json::object obj;
    obj["__type"] = "material";
    if (ecs::registry.all_of<scn::name_component>(ent))
    {
        auto& name = ecs::registry.get<scn::name_component>(ent);
        obj["name"] = name.name;
    }

    if (ecs::registry.all_of<scn::base_material_component>(ent))
    {
        auto& base_material = ecs::registry.get<scn::base_material_component>(ent);
        obj["base_material"] = boost::json::value_from(base_material);
    }

    if (ecs::registry.all_of<scn::transparent_material_component>(ent))
    {
        auto& transparent_material = ecs::registry.get<scn::transparent_material_component>(ent);
        obj["transparent_material"] = boost::json::value_from(transparent_material);
    }

    if (ecs::registry.all_of<scn::reflective_material_component>(ent))
    {
        auto& reflective_material = ecs::registry.get<scn::reflective_material_component>(ent);
        obj["reflective_material"] = boost::json::value_from(reflective_material);
    }

    if (ecs::registry.all_of<scn::albedo_map_component>(ent))
    {
        auto& albedo_map = ecs::registry.get<scn::albedo_map_component>(ent);
        obj["albedo_map"] = albedo_map.txm.get_full();
    }   

    if (ecs::registry.all_of<scn::normal_map_component>(ent))
    {
        auto& normal_map = ecs::registry.get<scn::normal_map_component>(ent);
        obj["normal_map"] = normal_map.txm.get_full();
    }   

    if (ecs::registry.all_of<scn::roughness_map_component>(ent))
    {
        auto& roughness_map = ecs::registry.get<scn::roughness_map_component>(ent);
        obj["roughness_map"] = roughness_map.txm.get_full();
    }   

    if (ecs::registry.all_of<scn::metallic_map_component>(ent))
    {
        auto& metallic_map = ecs::registry.get<scn::metallic_map_component>(ent);
        obj["metallic_map"] = metallic_map.txm.get_full();
    }      

    if (ecs::registry.all_of<scn::ao_map_component>(ent))
    {
        auto& ao_map = ecs::registry.get<scn::ao_map_component>(ent);
        obj["ao_map"] = ao_map.txm.get_full();
    }      

    if (ecs::registry.all_of<scn::height_map_component>(ent))
    {
        auto& height_map = ecs::registry.get<scn::height_map_component>(ent);
        obj["height_map"] = height_map.txm.get_full();
    }         

    if (ecs::registry.all_of<scn::specular_map_component>(ent))
    {
        auto& specular_map = ecs::registry.get<scn::specular_map_component>(ent);
        obj["specular_map"] = specular_map.txm.get_full();
    }      

    if (ecs::registry.all_of<scn::glossiness_map_component>(ent))
    {
        auto& glossiness_map = ecs::registry.get<scn::glossiness_map_component>(ent);
        obj["glossiness_map"] = glossiness_map.txm.get_full();
    }         

    if (ecs::registry.all_of<scn::emissive_map_component>(ent))
    {
        auto& emissive_map = ecs::registry.get<scn::emissive_map_component>(ent);
        obj["emissive_map"] = emissive_map.txm.get_full();
    }            

    if (ecs::registry.all_of<scn::opacity_map_component>(ent))
    {
        auto& opacity_map = ecs::registry.get<scn::opacity_map_component>(ent);
        obj["opacity_map"] = opacity_map.txm.get_full();
    }   

    if (ecs::registry.all_of<scn::is_transparent_flag_component>(ent))
    {
        obj["is_transparent"] = true;
    }  

    return obj;
}










