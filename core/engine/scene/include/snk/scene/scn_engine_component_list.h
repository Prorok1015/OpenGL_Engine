#pragma once
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_spatial.h>

namespace scn
{
	template<class...>
	struct type_list
	{
	};

	// Every component scn::register_engine_components puts in the base catalog, in its order (#922). Module code walks
	// their storages inline, so the module layout key (gs_module_layout.h) takes their sizes from this list: a component
	// registered there and not listed here, or the reverse, fails the EngineComponentCatalog test.
	using engine_component_types = type_list<
		local_transform, name_component, node_id_component, parent_component, children_component, object_component,
		renderable, scene_anchor_component, world_root_component, spawned_root_component, directional_light,
		mouse_controller_component, screen_anchor_component, spatial_layers,
		world_transform, depth_level, screen_anchor_result, bone_matrices_component,
		mesh_component, geometry_component, material_desc_component, skeleton_component, skinning_component,
		bone_component, sky_component, camera_component,
		animation_controller_component, animation_collection_component, animated_node_component, prefab_instance,
		component_tracker>;
}
