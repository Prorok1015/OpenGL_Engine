#include <boost/test/unit_test.hpp>
#include "edt_level_controller.h"
#include <string>
#include <string_view>
#include <vector>

// Regression guard for the multi-world parallel-array desync (code-review blocker #1).
//
// Root cause: world_names used to be built from scn::context_set::for_each (which iterates an
// unordered_map in hash order), while world_descs/worlds_systems_list were built from the level
// desc (desc order). With >=2 worlds the two orderings diverged, so prefab_editor_context::close()
// attached one world's systems to another world. The fix derives all three arrays from one ordered
// source via edt::derive_world_arrays. These tests pin the helper's alignment/order invariant.
//
// The helper is templated on a "world-like" type (get_name() + get_systems()), so we test it with a
// trivial stub instead of scn::world_desc — the latter can only be populated through the desc/resource
// pipeline (resource_system asserts on a real resources path), which the project's tests deliberately avoid.

namespace
{
	struct stub_world
	{
		std::string name;
		std::vector<std::string> systems;

		std::string_view get_name() const { return name; }
		const std::vector<std::string>& get_systems() const { return systems; }
	};
}

BOOST_AUTO_TEST_SUITE(EdtWorldArraySyncTests)

BOOST_AUTO_TEST_CASE(DeriveWorldArrays_PreservesInputOrderAndAligns)
{
	std::vector<stub_world> worlds = {
		{ "World_Z", { "RenderSystem", "PhysicsSystem" } },
		{ "World_A", { "InputSystem" } },
		{ "World_M", {} },
	};

	std::vector<std::string> names;
	std::vector<std::vector<std::string>> systems;
	edt::derive_world_arrays(worlds, names, systems);

	BOOST_REQUIRE_EQUAL(names.size(), worlds.size());
	BOOST_REQUIRE_EQUAL(systems.size(), worlds.size());

	for (size_t i = 0; i < worlds.size(); ++i) {
		// names[i] and systems[i] must refer to the SAME world as worlds[i] (index alignment),
		// and the input (desc) order must be preserved.
		BOOST_CHECK_EQUAL(names[i], worlds[i].name);
		BOOST_REQUIRE_EQUAL(systems[i].size(), worlds[i].systems.size());
		for (size_t j = 0; j < worlds[i].systems.size(); ++j)
			BOOST_CHECK_EQUAL(systems[i][j], worlds[i].systems[j]);
	}
}

BOOST_AUTO_TEST_CASE(DeriveWorldArrays_ClearsStaleOutput)
{
	std::vector<stub_world> worlds = { { "Only", { "Sys" } } };

	std::vector<std::string> names = { "stale1", "stale2" };
	std::vector<std::vector<std::string>> systems = { { "old" }, { "old2" } };
	edt::derive_world_arrays(worlds, names, systems);

	BOOST_REQUIRE_EQUAL(names.size(), 1u);
	BOOST_CHECK_EQUAL(names[0], "Only");
	BOOST_REQUIRE_EQUAL(systems.size(), 1u);
	BOOST_REQUIRE_EQUAL(systems[0].size(), 1u);
	BOOST_CHECK_EQUAL(systems[0][0], "Sys");
}

BOOST_AUTO_TEST_CASE(DeriveWorldArrays_EmptyInputClearsOutput)
{
	std::vector<stub_world> worlds;
	std::vector<std::string> names = { "stale" };
	std::vector<std::vector<std::string>> systems = { { "old" } };
	edt::derive_world_arrays(worlds, names, systems);

	BOOST_CHECK(names.empty());
	BOOST_CHECK(systems.empty());
}

BOOST_AUTO_TEST_CASE(MergeReloadedWorlds_KeepsDescOrderThenUnsavedAliveWorlds)
{
	std::vector<stub_world> desc = { { "B", {} }, { "A", {} } };
	std::vector<stub_world> previous = { { "A", { "stale" } }, { "Unsaved", { "Sys" } }, { "Gone", {} } };

	auto merged = edt::merge_reloaded_worlds(desc, previous,
		[](const stub_world& w) { return w.name != "Gone"; });

	BOOST_REQUIRE_EQUAL(merged.size(), 3u);
	BOOST_CHECK_EQUAL(merged[0].name, "B");
	BOOST_CHECK_EQUAL(merged[1].name, "A");
	BOOST_CHECK(merged[1].systems.empty()); // the desc copy wins over the stale editor one
	BOOST_CHECK_EQUAL(merged[2].name, "Unsaved");

	std::vector<std::string> names;
	std::vector<std::vector<std::string>> systems;
	edt::derive_world_arrays(merged, names, systems);
	BOOST_CHECK_EQUAL(names.size(), systems.size());
	BOOST_CHECK_EQUAL(systems[2][0], "Sys");
}

BOOST_AUTO_TEST_SUITE_END()
