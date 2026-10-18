#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "desc_node_id.h"
#include <sstream>

// Format v2 of prefab_desc / world_desc / level_desc: children are an array of nodes with "__id" and
// "name", and the v1 -> v2 migration that upgrades older files when they are loaded.

namespace json = boost::json;

namespace {

constexpr const char* V1_WORLD = R"({
	"__type": "world_desc", "name": "w", "systems": [],
	"children": {
		"arm": { "transform": { "position": [1, 0, 0] },
		         "components": { "stat": { "hp": 3 } },
		         "children": { "hand": { "components": { "stat": { "__type": "stat_desc", "hp": 1 } } } } },
		"Light": { "__type": "directional_light_desc" },
		"ref": "memory://t/ref.desc"
	}
})";

struct v2_fixture : test_util::scene_env
{
	json::object migrated(const char* text)
	{
		auto obj = json::parse(text).as_object();
		descs->migrate_tree(obj, "test");
		return obj;
	}

	json::object saved(const scn::prefab_desc& d)
	{
		json::object out;
		d.serialize(out);
		return out;
	}
};

uint64_t id_of(const json::value& node)
{
	return desc::parse_node_id(node.at("__id").as_string()).value_or(0);
}

}

BOOST_FIXTURE_TEST_SUITE(DescPrefabV2Tests, v2_fixture)

BOOST_AUTO_TEST_CASE(Version_PrefabWorldAndLevelAreAtVersionTwo)
{
	BOOST_TEST(descs->current_version("prefab_desc") == 2);
	BOOST_TEST(descs->current_version("world_desc") == 2);
	BOOST_TEST(descs->current_version("level_desc") == 2);
	BOOST_TEST(descs->current_version("camera_desc") == 1);
}

BOOST_AUTO_TEST_CASE(Migration_ChildrenObjectBecomesAnArrayWithNamesAndDerivedIds)
{
	auto w = migrated(V1_WORLD);
	BOOST_TEST(w.at("__version").as_int64() == 2);

	const auto& children = w.at("children").as_array();
	BOOST_REQUIRE(children.size() == 3u);
	BOOST_TEST(children[0].at("name").as_string() == "arm");
	BOOST_TEST(children[1].at("name").as_string() == "Light");
	BOOST_TEST(children[2].at("name").as_string() == "ref");

	BOOST_TEST(id_of(children[0]) == desc::derive_node_id("/arm"));
	const auto& hand = children[0].at("children").as_array().at(0);
	BOOST_TEST(hand.at("name").as_string() == "hand");
	BOOST_TEST(id_of(hand) == desc::derive_node_id("/arm/hand"));
	BOOST_TEST(id_of(w) == desc::derive_node_id(""));
}

BOOST_AUTO_TEST_CASE(Migration_OneLineChildrenBecomeNodesWithOneComponent)
{
	auto w = migrated(V1_WORLD);
	const auto& light = w.at("children").as_array()[1];
	BOOST_TEST(light.at("components").at("Light").at("__type").as_string() == "directional_light_desc");
	const auto& ref = w.at("children").as_array()[2];
	BOOST_TEST(ref.at("components").at("ref").as_string() == "memory://t/ref.desc");
}

BOOST_AUTO_TEST_CASE(Migration_ComponentTypeIsSpelledOut)
{
	auto w = migrated(V1_WORLD);
	const auto& stat = w.at("children").as_array()[0].at("components").at("stat");
	BOOST_TEST(stat.at("__type").as_string() == "stat");
	BOOST_TEST(stat.at("hp").to_number<int>() == 3);
}

BOOST_AUTO_TEST_CASE(Migration_KeepsAValidIdAndRepairsAMalformedOne)
{
	auto p = migrated(R"({"__type":"prefab_desc","children":{
		"a":{"__id":"00000000000000aa"}, "b":{"__id":"xyz"}}})");
	const auto& children = p.at("children").as_array();
	BOOST_TEST(id_of(children[0]) == 0xaaull);
	BOOST_TEST(id_of(children[1]) == desc::derive_node_id("/b"));
}

BOOST_AUTO_TEST_CASE(Migration_LeavesVersionTwoFilesAlone)
{
	const char* text = R"({"__type":"prefab_desc","__version":2,"children":[{"__id":"00000000000000aa","name":"a"}]})";
	auto p = migrated(text);
	BOOST_TEST(json::value(p) == json::parse(text));
}

BOOST_AUTO_TEST_CASE(Migration_NestedTypedPrefabAndWorldsInsideALevelAreMigratedToo)
{
	auto lv = migrated(R"({"__type":"level_desc","worlds":[
		{"__type":"world_desc","name":"w","children":{
			"n":{"components":{"inst":{"__type":"prefab_desc","children":{"k":{}}}}}}}]})");
	BOOST_TEST(lv.at("__version").as_int64() == 2);
	const auto& world = lv.at("worlds").as_array()[0];
	BOOST_TEST(world.at("__version").as_int64() == 2);
	const auto& inner = world.at("children").as_array()[0].at("components").at("inst");
	BOOST_TEST(inner.at("__version").as_int64() == 2);
	BOOST_TEST(inner.at("children").as_array()[0].at("name").as_string() == "k");
}

BOOST_AUTO_TEST_CASE(Load_V1AndV2FilesOfTheSameContentGiveTheSameTree)
{
	put("ref.desc", R"({"__type":"stat_desc","hp":5})");
	auto from_v1 = load<scn::world_desc>(put("v1.desc", V1_WORLD));
	BOOST_REQUIRE(from_v1);

	json::object v2_text = migrated(V1_WORLD);
	auto from_v2 = load<scn::world_desc>(put("v2.desc", json::serialize(v2_text)));
	BOOST_REQUIRE(from_v2);

	BOOST_TEST(json::value(saved(*from_v1)) == json::value(saved(*from_v2)));
	BOOST_REQUIRE(from_v1->get_root().children.size() == 3u);
	BOOST_TEST(from_v1->get_root().children[0].node_id == desc::derive_node_id("/arm"));
}

BOOST_AUTO_TEST_CASE(Load_V1LevelWithInlineWorldsMigrates)
{
	auto lv = load<scn::level_desc>(put("lv.desc", R"({"__type":"level_desc","name":"L","worlds":[
		{"__type":"world_desc","name":"w","systems":[],"children":{"c":{"components":{"stat":{"__type":"stat_desc","hp":9}}}}}]})"));
	BOOST_REQUIRE(lv);
	BOOST_REQUIRE(lv->get_worlds().size() == 1u);
	const auto& root = lv->get_worlds()[0]->get_root();
	BOOST_REQUIRE(root.children.size() == 1u);
	BOOST_TEST(root.children[0].name == "c");
	BOOST_TEST(root.children[0].components.at("stat").type_name == "stat_desc");
}

BOOST_AUTO_TEST_CASE(Save_WritesChildrenAsAnArrayWithExplicitTypes)
{
	auto w = load<scn::world_desc>(put("v1.desc", V1_WORLD));
	BOOST_REQUIRE(w);
	auto out = saved(*w);

	BOOST_REQUIRE(out.at("children").is_array());
	const auto& arm = out.at("children").as_array()[0];
	BOOST_TEST(arm.at("name").as_string() == "arm");
	BOOST_TEST(arm.at("__id").is_string());
	BOOST_TEST(arm.at("components").at("stat").at("__type").as_string() == "stat");
	BOOST_TEST(out.at("name").as_string() == "w"); // the world name; the root node has no name of its own
	// no one-line form any more: every child is a node object
	for (const auto& child : out.at("children").as_array()) {
		BOOST_TEST(child.is_object());
	}
}

BOOST_AUTO_TEST_CASE(Save_ComponentsAreWrittenInSortedOrder)
{
	scn::prefab_desc d;
	d.deserialize(*descs, json::parse(R"({"components":{
		"zeta":{"__type":"stat_desc"},"alpha":{"__type":"stat_desc"},"mid":{"__type":"stat_desc"}}})").as_object());
	const auto out = saved(d);
	std::vector<std::string> keys;
	for (auto& kv : out.at("components").as_object()) keys.emplace_back(kv.key());
	BOOST_TEST((keys == std::vector<std::string>{ "alpha", "mid", "zeta" }));
}

BOOST_AUTO_TEST_CASE(Duplicates_SiblingsWithTheSameNameAreKeptApart)
{
	scn::prefab_desc d;
	d.deserialize(*descs, json::parse(R"({"children":[
		{"__id":"00000000000000a1","name":"x","components":{"stat":{"__type":"stat_desc","hp":1}}},
		{"__id":"00000000000000a2","name":"x","components":{"stat":{"__type":"stat_desc","hp":2}}},
		{"name":"y"}, {"name":"y"}]})").as_object());
	const auto& children = d.get_root().children;
	BOOST_REQUIRE(children.size() == 4u);
	BOOST_TEST(children[0].node_id == 0xa1ull);
	BOOST_TEST(children[1].node_id == 0xa2ull);
	BOOST_TEST(children[2].node_id == desc::derive_node_id("/y"));
	BOOST_TEST(children[3].node_id == desc::derive_node_id("/y#1"));

	scn::prefab_desc again;
	again.deserialize(*descs, saved(d));
	BOOST_REQUIRE(again.get_root().children.size() == 4u);
	BOOST_TEST(again.get_root().children[1].components.at("stat").overrides.at("hp").as_int64() == 2);
	BOOST_TEST(json::value(saved(d)) == json::value(saved(again)));
}

BOOST_AUTO_TEST_CASE(ParentLayer_PairsNodesById)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000b1","name":"a","components":{"stat":{"__type":"stat_desc","hp":1}}},
		{"__id":"00000000000000b2","name":"b"}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":2,
		"__parent":"memory://t/base.prefab.desc","children":[
		{"__id":"00000000000000b1","name":"a_renamed","components":{"stat":{"__type":"stat_desc","hp":7}}},
		{"__id":"00000000000000c1","name":"b"}]})"));
	BOOST_REQUIRE(variant);
	const auto& children = variant->get_root().children;
	BOOST_REQUIRE(children.size() == 3u);
	BOOST_TEST(children[0].name == "a_renamed"); // same id: replaced in place
	BOOST_TEST(children[1].name == "b");
	BOOST_TEST(children[1].node_id == 0xb2ull);
	BOOST_TEST(children[2].node_id == 0xc1ull);  // same name, other id: a different node
}

BOOST_AUTO_TEST_CASE(PrettyWriter_KeepsNumberArraysOnOneLine)
{
	json::value v = json::parse(R"({"a":{"position":[1,2,3],"list":[{"x":1},{"x":2}]},"empty":[]})");
	std::ostringstream ss;
	descs->write_pretty_json(ss, v);
	const auto text = ss.str();
	BOOST_TEST(text.find("[1, 2, 3]") != std::string::npos);
	BOOST_TEST(text.find("[]") != std::string::npos);
	BOOST_TEST(json::value(json::parse(text)) == v);
}

BOOST_AUTO_TEST_CASE(StampTree_StampsNestedTypedObjectsAndOrdersTheHeader)
{
	json::object obj = json::parse(R"({"children":[],"__guid":"g","name":"n","__type":"world_desc",
		"inner":[{"x":{"__type":"prefab_desc"}}, {"__type":"camera_desc"}]})").as_object();
	descs->stamp_tree(obj);

	std::vector<std::string> keys;
	for (auto& kv : obj) keys.emplace_back(kv.key());
	BOOST_TEST((keys == std::vector<std::string>{ "__type", "__version", "__guid", "children", "name", "inner" }));
	BOOST_TEST(obj.at("__version").as_int64() == 2);
	BOOST_TEST(obj.at("inner").as_array()[0].at("x").at("__version").as_int64() == 2);
	BOOST_TEST(obj.at("inner").as_array()[1].at("__version").as_int64() == 1);
}

BOOST_AUTO_TEST_SUITE_END()
