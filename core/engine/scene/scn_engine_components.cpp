#include <snk/scene/scn_component_catalog.h>
#include <snk/common/engine_assert.h>
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_screen_anchor.h>

// The engine's runtime components in the base catalog (HOT_RELOAD_PLAY.md section 4.3). Each schema is described here,
// from outside the component: no member, base or virtual function is added to it.

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
	BOOST_DESCRIBE_STRUCT(directional_light, (), (direction, diffuse, ambient, specular))
	BOOST_DESCRIBE_STRUCT(mouse_controller_component, (), (movement_speed, rotating_speed, distance, rotation, position,
		last_mouse_pos, is_moving, is_rotating, input_enabled))
	BOOST_DESCRIBE_STRUCT(screen_anchor_component, (), (target, offset))
}

namespace
{
	template<typename T>
	void add(scn::component_catalog& catalog, std::string_view name)
	{
		const auto* entry = catalog.add<T>(std::string{ name }, desc::bind_described<T>(name, 1, desc::schema_kind::component));
		ASSERT_MSG(entry, "engine component '{}' is registered twice", name);
	}

	template<typename T>
	void add_derived(scn::component_catalog& catalog, std::string_view name)
	{
		const auto* entry = catalog.add_derived<T>(std::string{ name });
		ASSERT_MSG(entry, "engine component '{}' is registered twice", name);
	}

	// Registered so that a snapshot knows them, without a schema until #599 PR 3 describes them: a snapshot reports
	// them as lost until then.
	template<typename T>
	void add_pending(scn::component_catalog& catalog, std::string_view name)
	{
		catalog.add<T>(std::string{ name }, nullptr);
	}
}

void scn::register_engine_components(component_catalog& catalog)
{
	add<local_transform>(catalog, "scn.local_transform");
	add<name_component>(catalog, "scn.name");
	add<node_id_component>(catalog, "scn.node_id");
	add<parent_component>(catalog, "scn.parent");
	add<children_component>(catalog, "scn.children");
	add<object_component>(catalog, "scn.object");
	add<renderable>(catalog, "scn.renderable");
	add<scene_anchor_component>(catalog, "scn.scene_anchor");
	add<directional_light>(catalog, "scn.directional_light");
	add<mouse_controller_component>(catalog, "scn.mouse_controller");
	add<screen_anchor_component>(catalog, "scn.screen_anchor");

	add_derived<world_transform>(catalog, "scn.world_transform");
	add_derived<depth_level>(catalog, "scn.depth_level");
	add_derived<screen_anchor_result>(catalog, "scn.screen_anchor_result");
	add_derived<bone_matrices_component>(catalog, "scn.bone_matrices");

	add_pending<mesh_component>(catalog, "scn.mesh");
	add_pending<geometry_component>(catalog, "scn.geometry");
	add_pending<material_desc_component>(catalog, "scn.material_desc");
	add_pending<skeleton_component>(catalog, "scn.skeleton");
	add_pending<skinning_component>(catalog, "scn.skinning");
	add_pending<bone_component>(catalog, "scn.bone");
	add_pending<sky_component>(catalog, "scn.sky");
	add_pending<camera_component>(catalog, "scn.camera");
	add_pending<animation_controller_component>(catalog, "scn.animation_controller");
	add_pending<animation_collection_component>(catalog, "scn.animation_collection");
	add_pending<animated_node_component>(catalog, "scn.animated_node");
	add_pending<prefab_instance>(catalog, "scn.prefab_instance");
	add_pending<component_tracker>(catalog, "scn.component_tracker");
}
