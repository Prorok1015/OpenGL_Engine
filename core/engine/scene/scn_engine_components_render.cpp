#include "scn_engine_components_detail.h"
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_desc.h>

// The render components of the engine catalog (see scn_engine_components.cpp).

namespace scn
{
	BOOST_DESCRIBE_STRUCT(mesh_view, (), (vx_begin, vx_end, ind_begin, ind_end))
	BOOST_DESCRIBE_STRUCT(mesh_component, (), (mesh))
	BOOST_DESCRIBE_STRUCT(geometry_component, (), (geom_tag))
	// `dirty` is left out: a restored material is compiled again.
	BOOST_DESCRIBE_STRUCT(material_desc_component, (), (mlt_desc))
	BOOST_DESCRIBE_STRUCT(skeleton_component, (), (skinning_tag, bone_count, show_skeleton))
	BOOST_DESCRIBE_STRUCT(skinning_component, (), (skinning_tag, skeleton_entity))
	BOOST_DESCRIBE_STRUCT(bone_component, (), (offset, index))
	BOOST_DESCRIBE_STRUCT(sky_component, (), (cube_map))
	// The viewport is handed out by the Game view and `wireframe` is a debug view of the editor: neither is saved.
	BOOST_DESCRIBE_STRUCT(camera_component, (), (fov, near_distance, far_distance, layer, clear, projection, ortho_size,
		color_targets, depth_target, tp_accum_target, tp_reveal_target))
}

namespace desc
{
	// A material tag, or an inline material written back inline (desc_ref_traits).
	template<>
	inline constexpr auto field_attrs<scn::material_desc_component> = attrs(
		f("mlt_desc").kind(field_kind::desc_ref).ref("material_desc"));
}

void scn::detail::register_render_components(component_catalog& catalog)
{
	add_engine_component<mesh_component>(catalog, "scn.mesh");
	add_engine_component<geometry_component>(catalog, "scn.geometry");
	add_engine_component<material_desc_component>(catalog, "scn.material_desc");
	add_engine_component<skeleton_component>(catalog, "scn.skeleton");
	add_engine_component<skinning_component>(catalog, "scn.skinning");
	add_engine_component<bone_component>(catalog, "scn.bone");
	add_engine_component<sky_component>(catalog, "scn.sky");
	add_engine_component<camera_component>(catalog, "scn.camera");
}
