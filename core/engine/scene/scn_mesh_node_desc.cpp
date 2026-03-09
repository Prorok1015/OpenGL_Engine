#include "scn_mesh_node_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"

void scn::mesh_node_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("geometry"))
		geometry = desc_system.get_field_desc<rnd::geometry_desc>(*this, *p);
	if (auto const* p = data.if_contains("vx_begin"))
		vx_begin = p->to_number<std::size_t>();
	if (auto const* p = data.if_contains("vx_end"))
		vx_end = p->to_number<std::size_t>();
	if (auto const* p = data.if_contains("ind_begin"))
		ind_begin = p->to_number<std::size_t>();
	if (auto const* p = data.if_contains("ind_end"))
		ind_end = p->to_number<std::size_t>();
	if (auto const* p = data.if_contains("material"))
		material = desc_system.get_field_desc<scn::material_desc>(*this, *p);
}

void scn::mesh_node_desc::serialize(json::object& data) const
{
	if (geometry.is_valid())
		data["geometry"] = json::value_from(geometry->get_tag());
	data["vx_begin"] = vx_begin;
	data["vx_end"] = vx_end;
	data["ind_begin"] = ind_begin;
	data["ind_end"] = ind_end;
	if (material.is_valid())
		data["material"] = json::value_from(material->get_tag());
}

void scn::assemble_mesh_node(entt::registry& reg, entt::entity e, const mesh_node_desc& desc, const std::string_view name)
{
	scn::mesh_view mesh_data;
	mesh_data.vx_begin = desc.vx_begin;
	mesh_data.vx_end = desc.vx_end;
	mesh_data.ind_begin = desc.ind_begin;
	mesh_data.ind_end = desc.ind_end;

	reg.emplace_or_replace<scn::mesh_component>(e, scn::mesh_component{ .mesh = mesh_data });

	if (desc.geometry.is_valid())
		reg.emplace_or_replace<scn::geometry_component>(e, scn::geometry_component{ .geom_tag = desc.geometry->get_tag() });

	if (desc.material.is_valid())
		reg.emplace_or_replace<scn::material_desc_component>(e, scn::material_desc_component{ .mlt_desc = desc.material });

	reg.emplace_or_replace<scn::renderable>(e);
}
