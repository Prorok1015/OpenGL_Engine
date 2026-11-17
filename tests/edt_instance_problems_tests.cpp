#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_instance_problems.h"
#include <snk/scene/level/scn_world_desc.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

// #150: what is wrong with the prefab instances of the open level, as rows of Problems.

namespace {

constexpr const char* FIRST = "memory://t/first.prefab.desc";
constexpr const char* SECOND = "memory://t/second.prefab.desc";

struct instance_problems_fixture : test_util::scene_env
{
	std::shared_ptr<scn::world_desc> world_with(std::string_view children, std::string_view file_name = "w.desc")
	{
		std::string text = R"({"__type":"world_desc","__version":4,"name":"w","systems":[],"children":[)";
		text += children;
		text += "]}";
		return load<scn::world_desc>(put(file_name, text));
	}

	void put_cycle()
	{
		put("first.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
			{"__id":"00000000000000a1","name":"x","components":{"ref":"memory://t/second.prefab.desc"}}]})");
		put("second.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
			{"__id":"00000000000000b1","name":"y","components":{"ref":"memory://t/first.prefab.desc"}}]})");
	}

	// Two files that refer to each other give descs that hold handles to one another: taken apart when the test is done, or the
	// leak check of the sanitizer run reports the cycle (as scn_prefab_merge_tests does, #958).
	void take_apart(const char* name)
	{
		const auto prefab = load<scn::prefab_desc>(tag_of(name));
		if (!prefab) return;
		prefab->get_root().children.clear();
		prefab->get_root().components.clear();
	}

	void take_cycle_apart()
	{
		take_apart("first.prefab.desc");
		take_apart("second.prefab.desc");
	}

	static std::vector<res::diagnostic> diagnostics_of(const scn::world_desc& world, std::string file = "memory://t/w.desc", std::string prefix = {})
	{
		const std::vector<edt::instance_world> worlds{ { &world, std::move(file), std::move(prefix) } };
		return edt::instance_diagnostics(worlds);
	}

	static std::vector<res::diagnostic> cycles_of(const std::vector<res::diagnostic>& rows)
	{
		std::vector<res::diagnostic> out;
		std::ranges::copy_if(rows, std::back_inserter(out), [](const res::diagnostic& row) { return row.kind == res::diagnostic_kind::cycle; });
		return out;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtInstanceProblemsTests, instance_problems_fixture)

BOOST_AUTO_TEST_CASE(ALevelWithoutAnythingOdd_HasNoRows)
{
	put("leaf.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000f1","name":"tip"}]})");
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/leaf.prefab.desc"},
		"overrides":[{"target":["00000000000000f1"],"component":"transform","path":"/position","value":[0,1,0]}]})");
	BOOST_REQUIRE(world);

	BOOST_TEST(diagnostics_of(*world).empty());
}

BOOST_AUTO_TEST_CASE(ARemoveNodeRecord_IsAWarningAtItsNode_NamingTheRecord)
{
	put("leaf.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000f1","name":"tip"}]})");
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/leaf.prefab.desc"},
		"overrides":[{"op":"remove","target":["00000000000000f1"]}]})");
	BOOST_REQUIRE(world);

	const auto rows = diagnostics_of(*world);

	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST((rows[0].severity == res::diagnostic_severity::warning));
	BOOST_TEST(rows[0].json_path == "/children/0");
	BOOST_TEST(rows[0].resource == "memory://t/w.desc");
	BOOST_TEST(rows[0].reason.find("remove node f1") != std::string::npos);
	BOOST_TEST(rows[0].reason.find("'N'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ARemoveComponentRecordAndAnAddNodeRecord_AreRowsAtTheirNodes_ASetRecordIsNone)
{
	put("leaf.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000f1","name":"tip"}]})");
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/leaf.prefab.desc"},
		"overrides":[{"op":"remove","target":["00000000000000f1"],"component":"stat"},
			{"op":"add_node","target":[],"value":{"__id":"00000000000000d9","name":"extra"}},
			{"target":["00000000000000f1"],"component":"stat","path":"/hp","value":3}]},
		{"__id":"00000000000000d2","name":"M","components":{"ref":"memory://t/leaf.prefab.desc"},
		"overrides":[{"op":"add_node","target":[],"value":{"__id":"00000000000000d8","name":"more"}}]})");
	BOOST_REQUIRE(world);

	const auto rows = diagnostics_of(*world);

	BOOST_REQUIRE(rows.size() == 3u);
	BOOST_TEST(rows[0].reason.find("remove component stat") != std::string::npos);
	BOOST_TEST(rows[1].reason.find("add node 'extra'") != std::string::npos);
	BOOST_TEST(rows[0].json_path == "/children/0");
	BOOST_TEST(rows[1].json_path == "/children/0");
	BOOST_TEST(rows[2].reason.find("add node 'more'") != std::string::npos);
	BOOST_TEST(rows[2].json_path == "/children/1");
}

BOOST_AUTO_TEST_CASE(APrefabThatInstantiatesItselfThroughAnother_IsOneErrorWithTheChainAtItsNode)
{
	put_cycle();
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/first.prefab.desc"}})");
	BOOST_REQUIRE(world);

	const auto rows = diagnostics_of(*world);
	take_cycle_apart();

	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST((rows[0].severity == res::diagnostic_severity::error));
	BOOST_TEST(rows[0].kind == std::string(res::diagnostic_kind::cycle));
	BOOST_TEST(rows[0].json_path == "/children/0");
	BOOST_TEST(rows[0].reason == std::string("Prefab '") + FIRST + "' refers to itself through '" + SECOND + "' (" + FIRST + " -> " + SECOND + " -> " + FIRST + ")");
}

BOOST_AUTO_TEST_CASE(TheTextOfACycle_IsSaidFromThePrefabOfTheNode_WhateverNodeCameFirst)
{
	put_cycle();
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"M","components":{"ref":"memory://t/second.prefab.desc"}},
		{"__id":"00000000000000d2","name":"N","components":{"ref":"memory://t/first.prefab.desc"}})");
	BOOST_REQUIRE(world);

	const auto rows = diagnostics_of(*world);
	take_cycle_apart();

	BOOST_REQUIRE(rows.size() == 2u);
	BOOST_TEST(rows[0].json_path == "/children/0");
	BOOST_TEST(rows[0].reason == std::string("Prefab '") + SECOND + "' refers to itself through '" + FIRST + "' (" + SECOND + " -> " + FIRST + " -> " + SECOND + ")");
	BOOST_TEST(rows[1].json_path == "/children/1");
	BOOST_TEST(rows[1].reason == std::string("Prefab '") + FIRST + "' refers to itself through '" + SECOND + "' (" + FIRST + " -> " + SECOND + " -> " + FIRST + ")");
}

BOOST_AUTO_TEST_CASE(APrefabThatReferencesItsOwnFile_IsAnErrorWithoutAChain)
{
	put("self.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"again","components":{"ref":"memory://t/self.prefab.desc"}}]})");
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/self.prefab.desc"}})");
	BOOST_REQUIRE(world);

	const auto rows = cycles_of(diagnostics_of(*world));

	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(rows[0].reason == "Prefab 'memory://t/self.prefab.desc' refers to itself");
}

BOOST_AUTO_TEST_CASE(TheSameCycleInTwoWorlds_GivesARowAtTheNodeOfEach_AnInlineWorldGetsItsPointerInTheLevel)
{
	put_cycle();
	const auto first_world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/first.prefab.desc"}})", "w0.desc");
	const auto second_world = world_with(R"({"__id":"00000000000000d2","name":"N","components":{"ref":"memory://t/first.prefab.desc"}})", "w1.desc");
	BOOST_REQUIRE(first_world);
	BOOST_REQUIRE(second_world);
	const std::vector<edt::instance_world> worlds{
		{ first_world.get(), "memory://t/w0.desc", "" },
		{ second_world.get(), "memory://t/level.desc", "/worlds/1" } };

	const auto rows = edt::instance_diagnostics(worlds);
	take_cycle_apart();

	BOOST_REQUIRE(rows.size() == 2u);
	BOOST_TEST(rows[0].resource == "memory://t/w0.desc");
	BOOST_TEST(rows[0].json_path == "/children/0");
	BOOST_TEST(rows[1].resource == "memory://t/level.desc");
	BOOST_TEST(rows[1].json_path == "/worlds/1/children/0");
}

BOOST_AUTO_TEST_SUITE_END()
