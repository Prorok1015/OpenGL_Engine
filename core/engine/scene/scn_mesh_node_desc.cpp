#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_glm_json_convert.h>
#include <snk/common/logger/engine_log.h>
#include <string>
#include <string_view>

namespace {

template<typename T>
void read_reference(desc::desc_system& desc_system, const desc::desc_base& owner, const json::object& data, std::string_view key, res::res_handle<T>& field)
{
	const json::value* value = data.if_contains(key);
	if (!value) {
		return;
	}
	const json::object* inline_desc = value->if_object();
	if (!value->is_string() && !(inline_desc && inline_desc->contains("__type") && inline_desc->at("__type").is_string())) {
		egLOG_WARN("desc", "mesh_node_desc: '{}' is neither a tag nor an inline desc, keeping the current one", key);
		return;
	}
	field = desc_system.get_field_desc<T>(owner, *value, std::string("/").append(key));
}

}

void scn::mesh_node_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	// The assembler reads a component into an untagged copy, and the generic desc_ref reader refuses an inline desc
	// there; the importer writes the material inline, so the references are read as the hand-written reader did.
	json::object scalars = data;
	scalars.erase("geometry");
	scalars.erase("material");
	schema_desc::deserialize(desc_system, scalars);
	read_reference(desc_system, *this, data, "geometry", geometry);
	read_reference(desc_system, *this, data, "material", material);
}

// Written by hand too: the generic writer would write a memory:// reference as an inline copy (get_sync on the main thread),
// and the assembler's serialize(parent) + deserialize of an override would then make a second material instead of sharing
// the parent's.
void scn::mesh_node_desc::serialize(json::object& data) const
{
	if (geometry.get_tag().is_valid())
		data["geometry"] = json::value_from(geometry.get_tag());
	data["vx_begin"] = vx_begin;
	data["vx_end"] = vx_end;
	data["ind_begin"] = ind_begin;
	data["ind_end"] = ind_end;
	if (material.get_tag().is_valid())
		data["material"] = json::value_from(material.get_tag());
	if (!visible)
		data["visible"] = false;
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
