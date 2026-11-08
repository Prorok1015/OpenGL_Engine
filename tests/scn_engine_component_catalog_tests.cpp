#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/level/scn_screen_anchor.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <type_traits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Every runtime component the engine creates is in the base catalog with a schema, or marked derived / editor-only
// (#599, HOT_RELOAD_PLAY.md section 4.3). A world is spawned through the production spawners with a node per scene desc
// listed below (a hand-written list: a new desc type must be added to it), and every non-empty storage must be known:
// a component one of these spawners adds without registering it makes the walk report it, which fails the test.

namespace
{
	// Registered by #599 PR 2 without a schema; #599 PR 3 describes them and empties this list.
	const std::set<std::string, std::less<>> k_schema_pending = {
		"scn.mesh", "scn.geometry", "scn.material_desc", "scn.skeleton", "scn.skinning", "scn.bone", "scn.sky",
		"scn.camera", "scn.animation_controller", "scn.animation_collection", "scn.animated_node",
		"scn.prefab_instance", "scn.component_tracker",
	};

	struct catalog_fixture : test_util::scene_env
	{
		catalog_fixture() { scn::register_engine_components(catalog); }

		// A prefab with one node per scene desc listed here; a skinning node with bones adds the skeleton and the bone
		// matrices to the root.
		entt::entity spawn_every_desc(entt::registry& reg)
		{
			const auto tag = put("all.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
				{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":2}}},
				{"__id":"00000000000000a2","name":"Sun","components":{"light":{"__type":"directional_light_desc"}}},
				{"__id":"00000000000000a3","name":"Marker","components":{"marker":{"__type":"screen_anchor_desc"}}},
				{"__id":"00000000000000a4","name":"Sky","components":{"sky":{"__type":"skybox_desc"}}},
				{"__id":"00000000000000a5","name":"Mesh","components":{"mesh":{"__type":"mesh_node_desc"},
					"skin":{"__type":"skinning_desc","bone_count":2}}},
				{"__id":"00000000000000a6","name":"Object","components":{"object":{"__type":"object_desc"}}},
				{"__id":"00000000000000a7","name":"Bone","components":{"bone":{"__type":"bone_desc"}}},
				{"__id":"00000000000000a8","name":"Animator","components":{"controller":{"__type":"animation_controller_desc"},
					"clips":{"__type":"animation_collection_desc"}}},
				{"__id":"00000000000000a9","name":"Animated","components":{"node":{"__type":"animated_node_desc"}}},
				{"__id":"00000000000000aa","name":"Anchor","components":{"anchor":{"__type":"anchor_desc"}},
					"children":[{"__id":"00000000000000ab","name":"Child","components":{"object":{"__type":"object_desc"}}}]}
			]})");
			const entt::entity root = reg.create();
			scn::assemble_prefab(*assembler, reg, root, *load<scn::prefab_desc>(tag), "all", tag);
			return root;
		}

		scn::component_catalog catalog;
	};

	struct unregistered_component
	{
		int value = 0;
	};

	bool has_storage(const entt::registry& reg, entt::id_type id)
	{
		const auto* storage = reg.storage(id);
		return storage && !storage->empty();
	}
}

BOOST_FIXTURE_TEST_SUITE(EngineComponentCatalog, catalog_fixture)

BOOST_AUTO_TEST_CASE(EveryComponentOfASpawnedWorldIsInTheCatalog)
{
	entt::registry reg;
	spawn_every_desc(reg);

	for (const auto& missing : scn::unregistered_storages(reg, catalog)) {
		BOOST_ERROR("a spawned world holds '" << missing.type << "', which is not in the engine's component catalog");
	}
	// The walk saw the components the prefab is meant to produce.
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::camera_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::bone_matrices_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::parent_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::children_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::scene_anchor_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::renderable>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::directional_light>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::screen_anchor_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::skeleton_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::animated_node_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::mesh_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::animation_controller_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::object_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::sky_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::bone_component>::value()));
}

BOOST_AUTO_TEST_CASE(EveryEngineEntryHasASchemaOrIsMarked)
{
	catalog.for_each([](const scn::component_entry& entry) {
		const bool marked = entry.category == scn::component_category::derived || entry.category == scn::component_category::editor_only;
		if (entry.schema || marked) {
			BOOST_TEST(!k_schema_pending.contains(entry.name), entry.name << " has a schema now: take it off k_schema_pending");
			return;
		}
		BOOST_TEST(k_schema_pending.contains(entry.name), entry.name << " has no schema and is not marked derived or editor-only");
	});
}

BOOST_AUTO_TEST_CASE(AComponentOutsideTheCatalogIsReported)
{
	entt::registry reg;
	const entt::entity root = spawn_every_desc(reg);
	reg.emplace<unregistered_component>(root);

	const auto missing = scn::unregistered_storages(reg, catalog);
	BOOST_REQUIRE_EQUAL(missing.size(), 1u);
	BOOST_TEST(missing.front().id == entt::type_hash<unregistered_component>::value());
	BOOST_TEST(missing.front().type.find("unregistered_component") != std::string::npos);

	// A storage whose components are all gone is not a component of the world any more.
	reg.remove<unregistered_component>(root);
	BOOST_TEST(scn::unregistered_storages(reg, catalog).empty());
}

BOOST_AUTO_TEST_CASE(DescribingAComponentKeepsItsLayout)
{
	// The schemas are described from outside: the components keep their size (no member, base or vtable added).
	BOOST_TEST(sizeof(scn::local_transform) == sizeof(glm::mat4));
	BOOST_TEST(sizeof(scn::parent_component) == sizeof(entt::entity));
	BOOST_TEST(std::is_empty_v<scn::renderable>);
	BOOST_TEST(!std::is_polymorphic_v<scn::name_component>);
	BOOST_TEST(std::is_aggregate_v<scn::mouse_controller_component>);
}

BOOST_AUTO_TEST_CASE(ALayerSeesTheEngineComponents)
{
	scn::component_catalog layer{ &catalog };
	BOOST_REQUIRE(layer.find("scn.local_transform"));
	BOOST_TEST(layer.find(entt::type_hash<scn::local_transform>::value())->saved());
	BOOST_TEST(!layer.add<unregistered_component>("scn.local_transform", nullptr));
}

BOOST_AUTO_TEST_SUITE_END()
