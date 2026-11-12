#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_glm_json_convert.h>

void scn::mesh_node_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	schema_desc::deserialize(desc_system, data);
}

void scn::mesh_node_desc::serialize(json::object& data) const
{
	schema_desc::serialize(data);
}

void scn::assemble_mesh_node(entt::registry& reg, entt::entity e, const mesh_node_desc& desc, const std::string_view name)
{
	scn::mesh_view mesh_data;
	mesh_data.vx_begin = desc.vx_begin;
	mesh_data.vx_end = desc.vx_end;
	mesh_data.ind_begin = desc.ind_begin;
	mesh_data.ind_end = desc.ind_end;

	reg.emplace_or_replace<scn::mesh_component>(e, scn::mesh_component{ .mesh = mesh_data });

	if (desc.geometry.is_valid() && !desc.geometry.has_error() && desc.geometry.get_tag().is_valid())
		reg.emplace_or_replace<scn::geometry_component>(e, scn::geometry_component{ .geom_tag = desc.geometry.get_tag() });

	if (desc.material.is_valid())
		reg.emplace_or_replace<scn::material_desc_component>(e, scn::material_desc_component{ .mlt_desc = desc.material });

	scn::set_visible(reg, e, desc.visible);
}
