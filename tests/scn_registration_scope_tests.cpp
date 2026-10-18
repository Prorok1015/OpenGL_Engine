#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "scn_registration_scope.h"

namespace {

constexpr std::string_view scene_descs[] = {
	"material_desc", "prefab_desc", "world_desc", "level_desc", "anchor_desc", "camera_desc",
	"directional_light_desc", "skybox_desc", "mesh_node_desc", "object_desc", "bone_desc",
	"skinning_desc", "skin_weights_desc", "animation_controller_desc", "animated_node_desc",
	"animation_clip_desc", "animation_collection_desc",
};

constexpr std::string_view scene_spawners[] = {
	"prefab_desc", "world_desc", "anchor_desc", "camera_desc", "directional_light_desc",
	"skybox_desc", "mesh_node_desc", "object_desc", "bone_desc", "skinning_desc",
	"skin_weights_desc", "animation_controller_desc", "animated_node_desc",
	"animation_collection_desc",
};

struct scope_fixture : test_util::ram_res_env
{
	scope_fixture()
		: descs(*rs)
		, assembler(descs, *rs)
	{
	}

	desc::desc_system descs;
	scn::ecs_assembler assembler;
	rnd::skinning_manager skinning;
};

}

BOOST_AUTO_TEST_SUITE(scn_registration_scope_tests)

BOOST_FIXTURE_TEST_CASE(register_scene_types_registers_everything, scope_fixture)
{
	auto scope = scn::register_scene_types(descs, assembler, skinning);
	constexpr std::size_t migrations = 3; // prefab_desc, world_desc and level_desc, v1 -> v2
	BOOST_CHECK_EQUAL(scope.size(), std::size(scene_descs) + std::size(scene_spawners) + migrations);
	for (auto type : { "prefab_desc", "world_desc", "level_desc" }) BOOST_CHECK_EQUAL(descs.current_version(type), 2);
	for (auto type : scene_descs) BOOST_CHECK_MESSAGE(descs.is_registered(type), type);
	for (auto type : scene_spawners) BOOST_CHECK_MESSAGE(assembler.has_spawner(type), type);
	BOOST_CHECK_EQUAL(descs.get_component_type_names().size(), 3u);
}

BOOST_FIXTURE_TEST_CASE(destroying_scope_restores_registries, scope_fixture)
{
	{
		auto scope = scn::register_scene_types(descs, assembler, skinning);
		BOOST_REQUIRE(descs.is_registered("prefab_desc"));
	}
	for (auto type : scene_descs) BOOST_CHECK_MESSAGE(!descs.is_registered(type), type);
	for (auto type : scene_spawners) BOOST_CHECK_MESSAGE(!assembler.has_spawner(type), type);
	BOOST_CHECK(descs.get_component_type_names().empty());
	BOOST_CHECK_EQUAL(descs.current_version("prefab_desc"), 1); // migrations leave with the scope
}

BOOST_FIXTURE_TEST_CASE(scene_types_can_be_registered_again_after_scope_ends, scope_fixture)
{
	{ auto first = scn::register_scene_types(descs, assembler, skinning); }
	auto second = scn::register_scene_types(descs, assembler, skinning);
	BOOST_CHECK(descs.is_registered("world_desc"));
	BOOST_CHECK(assembler.has_spawner("world_desc"));
}

BOOST_FIXTURE_TEST_CASE(moved_scope_keeps_registrations_until_last_owner_dies, scope_fixture)
{
	std::optional<scn::registration_scope> outer;
	{
		auto inner = scn::register_scene_types(descs, assembler, skinning);
		outer.emplace(std::move(inner));
		BOOST_CHECK_EQUAL(inner.size(), 0u);
	}
	BOOST_CHECK(descs.is_registered("prefab_desc"));
	BOOST_CHECK(assembler.has_spawner("skin_weights_desc"));
	outer.reset();
	BOOST_CHECK(!descs.is_registered("prefab_desc"));
	BOOST_CHECK(!assembler.has_spawner("skin_weights_desc"));
}

BOOST_FIXTURE_TEST_CASE(scope_does_not_touch_foreign_registrations, scope_fixture)
{
	descs.register_desc<test_util::stat_desc>("stat_desc");
	{
		auto scope = scn::register_scene_types(descs, assembler, skinning);
	}
	BOOST_CHECK(descs.is_registered("stat_desc"));
}

BOOST_AUTO_TEST_SUITE_END()
