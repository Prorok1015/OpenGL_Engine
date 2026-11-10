#include "scn_engine_components_detail.h"
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_screen_anchor.h>

// The engine's runtime components in the base catalog (HOT_RELOAD_PLAY.md section 4.3). Each schema is described next
// to its registration, from outside the component: no member, base or virtual function is added to it. The render and
// the animation components are in scn_engine_components_render.cpp and scn_engine_components_animation.cpp.
// A component the engine creates on a world and does not register here fails CI (#620): the catalog test for the desc
// spawners, tools/ci/check_engine_components.py for the sources.

namespace scn
{
	BOOST_DESCRIBE_STRUCT(local_transform, (), (local))
	BOOST_DESCRIBE_STRUCT(name_component, (), (name))
	BOOST_DESCRIBE_STRUCT(node_id_component, (), (path))
	BOOST_DESCRIBE_STRUCT(parent_component, (), (parent))
	BOOST_DESCRIBE_STRUCT(children_component, (), (children))
	BOOST_DESCRIBE_STRUCT(object_component, (), ())
	BOOST_DESCRIBE_STRUCT(renderable, (), ())
	BOOST_DESCRIBE_STRUCT(scene_anchor_component, (), ())
	BOOST_DESCRIBE_STRUCT(world_root_component, (), ())
	BOOST_DESCRIBE_STRUCT(spawned_root_component, (), ())
	BOOST_DESCRIBE_STRUCT(directional_light, (), (direction, diffuse, ambient, specular))
	BOOST_DESCRIBE_STRUCT(mouse_controller_component, (), (movement_speed, rotating_speed, distance, rotation, position,
		last_mouse_pos, is_moving, is_rotating, input_enabled))
	BOOST_DESCRIBE_STRUCT(screen_anchor_component, (), (target, offset))
}

void scn::register_engine_components(component_catalog& catalog)
{
	using detail::add_engine_component;
	using detail::add_engine_derived;
	add_engine_component<local_transform>(catalog, "scn.local_transform");
	add_engine_component<name_component>(catalog, "scn.name");
	add_engine_component<node_id_component>(catalog, "scn.node_id");
	add_engine_component<parent_component>(catalog, "scn.parent");
	add_engine_component<children_component>(catalog, "scn.children");
	add_engine_component<object_component>(catalog, "scn.object");
	add_engine_component<renderable>(catalog, "scn.renderable");
	add_engine_component<scene_anchor_component>(catalog, "scn.scene_anchor");
	add_engine_component<world_root_component>(catalog, "scn.world_root");
	add_engine_component<spawned_root_component>(catalog, "scn.spawned_root");
	add_engine_component<directional_light>(catalog, "scn.directional_light");
	add_engine_component<mouse_controller_component>(catalog, "scn.mouse_controller");
	add_engine_component<screen_anchor_component>(catalog, "scn.screen_anchor");

	add_engine_derived<world_transform>(catalog, "scn.world_transform");
	add_engine_derived<depth_level>(catalog, "scn.depth_level");
	add_engine_derived<screen_anchor_result>(catalog, "scn.screen_anchor_result");
	add_engine_derived<bone_matrices_component>(catalog, "scn.bone_matrices");

	detail::register_render_components(catalog);
	detail::register_animation_components(catalog);
}
