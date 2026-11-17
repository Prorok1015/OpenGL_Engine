#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_instance_problems.h"
#include <snk/scene/level/scn_world_desc.h>
#include <algorithm>
#include <string>
#include <vector>

// #150: what is wrong with the prefab instances of the open level, as rows of Problems.

namespace {

struct instance_problems_fixture : test_util::scene_env
{
	std::shared_ptr<scn::world_desc> world_with(std::string_view children)
	{
		std::string text = R"({"__type":"world_desc","__version":4,"name":"w","systems":[],"children":[)";
		text += children;
		text += "]}";
		return load<scn::world_desc>(put("w.desc", text));
	}

	std::vector<res::diagnostic> diagnostics_of_world(const scn::world_desc& world)
	{
		const std::vector<edt::instance_world> worlds{ { &world, "memory://t/w.desc" } };
		return edt::instance_diagnostics(worlds);
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

	BOOST_TEST(diagnostics_of_world(*world).empty());
}

BOOST_AUTO_TEST_CASE(ARemoveRecordOnAnInstance_IsAWarningAtItsNode_NamingTheRecord)
{
	put("leaf.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000f1","name":"tip"}]})");
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/leaf.prefab.desc"},
		"overrides":[{"op":"remove","target":["00000000000000f1"]}]})");
	BOOST_REQUIRE(world);

	const auto rows = diagnostics_of_world(*world);

	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST((rows[0].severity == res::diagnostic_severity::warning));
	BOOST_TEST(rows[0].json_path == "/children/0");
	BOOST_TEST(rows[0].resource == "memory://t/w.desc");
	BOOST_TEST(rows[0].reason.find("remove node f1") != std::string::npos);
	BOOST_TEST(rows[0].reason.find("'N'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(APrefabThatInstantiatesItselfThroughAnother_IsOneErrorNamingTheChain)
{
	put("first.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"x","components":{"ref":"memory://t/second.prefab.desc"}}]})");
	put("second.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"y","components":{"ref":"memory://t/first.prefab.desc"}}]})");
	const auto world = world_with(R"({"__id":"00000000000000d1","name":"N","components":{"ref":"memory://t/first.prefab.desc"}},
		{"__id":"00000000000000d2","name":"M","components":{"ref":"memory://t/second.prefab.desc"}})");
	BOOST_REQUIRE(world);

	const auto rows = diagnostics_of_world(*world);

	const auto cycles = std::ranges::count_if(rows, [](const res::diagnostic& row) { return row.kind == res::diagnostic_kind::cycle; });
	BOOST_TEST(cycles >= 1);
	BOOST_TEST(std::ranges::any_of(rows, [](const res::diagnostic& row) {
		return row.reason.find("refers to itself through") != std::string::npos && row.reason.find("first.prefab.desc") != std::string::npos
			&& row.reason.find("second.prefab.desc") != std::string::npos;
	}));
}

BOOST_AUTO_TEST_SUITE_END()
