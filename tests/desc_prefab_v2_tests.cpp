#include <snk/scene/level/scn_prefab_layer.h>
#include <snk/scene/level/scn_override_record.h>
#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/desc_node_id.h>
#include <functional>
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

// splitmix64: the same sequence on every standard library (std::uniform_*_distribution and std::shuffle are not).
struct splitmix64
{
	uint64_t state;
	uint64_t next()
	{
		uint64_t z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

json::object random_node(splitmix64& rng, uint64_t& next_id, int depth)
{
	json::object node;
	node["__id"] = desc::format_node_id(next_id++);
	node["name"] = "n" + std::to_string(rng.below(1000));
	if (rng.below(2) == 0) {
		json::object transform;
		transform["position"] = json::array{ static_cast<double>(rng.below(9)), 0.0, 1.0 };
		node["transform"] = std::move(transform);
	}
	if (rng.below(2) == 0) {
		node["components"] = json::object{ { "stat", json::object{ { "__type", "stat_desc" }, { "hp", static_cast<int64_t>(rng.below(50)) } } } };
	}
	if (rng.below(3) == 0) {
		json::object record;
		record["target"] = json::array{ node.at("__id") };
		record["component"] = "stat";
		record["path"] = "/hp";
		record["value"] = static_cast<int64_t>(rng.below(50));
		record["op"] = "set";
		node["overrides"] = json::array{ std::move(record) };
	}
	json::array children;
	const std::size_t count = depth >= 2 ? 0 : rng.below(4);
	for (std::size_t i = 0; i < count; ++i) children.push_back(random_node(rng, next_id, depth + 1));
	node["children"] = std::move(children);
	return node;
}

void collect_nodes(scn::prefab_desc::prefab_node& node, std::vector<scn::prefab_desc::prefab_node*>& out)
{
	out.push_back(&node);
	for (auto& child : node.children) collect_nodes(child, out);
}

void mutate(splitmix64& rng, scn::prefab_desc& edited, uint64_t& next_id)
{
	for (int step = 0, steps = 1 + static_cast<int>(rng.below(5)); step < steps; ++step) {
		std::vector<scn::prefab_desc::prefab_node*> nodes;
		collect_nodes(edited.get_root(), nodes);
		auto& node = *nodes[rng.below(nodes.size())];
		switch (rng.below(9)) {
		case 0: if (&node != &edited.get_root()) node.name = "renamed" + std::to_string(rng.below(100)); break;
		case 1: node.has_transform = true; node.position = glm::vec3(static_cast<float>(rng.below(20)), 0.f, 0.f); break;
		case 2: node.components.erase("stat"); break;
		case 3: {
			scn::prefab_desc::prefab_comp_node comp;
			comp.type_name = "stat_desc";
			comp.overrides["hp"] = static_cast<int64_t>(100 + rng.below(50));
			node.components["stat"] = std::move(comp);
			break;
		}
		case 4: if (!node.children.empty()) node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(rng.below(node.children.size()))); break;
		case 5: {
			scn::prefab_desc::prefab_node added;
			added.node_id = next_id++;
			added.name = "added";
			node.children.push_back(std::move(added));
			break;
		}
		case 6: {
			scn::override_record record;
			record.target = { node.node_id };
			record.component = "stat";
			record.path = "/hp";
			record.value = static_cast<int64_t>(200 + rng.below(50));
			node.overrides.push_back(std::move(record));
			break;
		}
		case 7: node.has_transform = false; node.position = glm::vec3(0.f); break;
		default:
			if (node.children.size() > 1) std::swap(node.children.front(), node.children[rng.below(node.children.size())]);
			break;
		}
	}
}

}

BOOST_FIXTURE_TEST_SUITE(DescPrefabV2Tests, v2_fixture)

BOOST_AUTO_TEST_CASE(Version_PrefabAndWorldAreAtVersionFourLevelAtTwoAndCameraAtTwo)
{
	BOOST_TEST(descs->current_version("prefab_desc") == 4);
	BOOST_TEST(descs->current_version("world_desc") == 4);
	BOOST_TEST(descs->current_version("level_desc") == 2);
	BOOST_TEST(descs->current_version("camera_desc") == 2);
}

BOOST_AUTO_TEST_CASE(Migration_ChildrenObjectBecomesAnArrayWithNamesAndDerivedIds)
{
	auto w = migrated(V1_WORLD);
	BOOST_TEST(w.at("__version").as_int64() == 4);

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

BOOST_AUTO_TEST_CASE(Migration_LeavesCurrentVersionFilesAlone)
{
	const char* text = R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000aa","name":"a"}]})";
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
	BOOST_TEST(world.at("__version").as_int64() == 4);
	const auto& inner = world.at("children").as_array()[0].at("components").at("inst");
	BOOST_TEST(inner.at("__version").as_int64() == 4);
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
	BOOST_TEST(lv->get_name() == "L");

	// the level name survives a save through the adapter
	core::desc::desc_adapter_t adapter(*rs, *descs);
	auto bytes = adapter.serialize(tag_of("lv.desc"), lv);
	auto text = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	BOOST_TEST(json::parse(text).at("name").as_string() == "L");
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

BOOST_AUTO_TEST_CASE(LayerV2_RemovesAddsOrdersAndEditsOnlyWhatItNames)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"transform":{"position":[1,2,3]},"children":[
		{"__id":"00000000000000b1","name":"a","components":{"stat":{"__type":"stat_desc","hp":1}}},
		{"__id":"00000000000000b2","name":"b"},
		{"__id":"00000000000000b3","name":"c"}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"transform":{"scale":[2,2,2]},
		"__order":["00000000000000b3"],"children":[
		{"__id":"00000000000000b2","__remove":true},
		{"__id":"00000000000000b1","components":{"stat":null}},
		{"__id":"00000000000000d1","__new":true,"name":"d"}]})"));
	BOOST_REQUIRE(variant);
	const auto& root = variant->get_root();
	BOOST_TEST(root.position == glm::vec3(1, 2, 3));
	BOOST_TEST(root.scale == glm::vec3(2, 2, 2));
	BOOST_REQUIRE(root.children.size() == 3u);
	BOOST_TEST(root.children[0].node_id == 0xb3ull);
	BOOST_TEST(root.children[1].node_id == 0xb1ull);
	BOOST_TEST(root.children[1].components.empty());
	BOOST_TEST(root.children[2].node_id == 0xd1ull);
	BOOST_TEST(root.children[2].name == "d");
}

BOOST_AUTO_TEST_CASE(LayerV2_NullTransformMeansNoTransform_NullFieldMeansDefault)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","transform":{"position":[4,5,6],"scale":[3,3,3]}},
		{"__id":"00000000000000b2","name":"b","transform":{"position":[1,1,1]}}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[
		{"__id":"00000000000000b1","transform":{"scale":null}},
		{"__id":"00000000000000b2","transform":null}]})"));
	BOOST_REQUIRE(variant);
	const auto& children = variant->get_root().children;
	BOOST_TEST(children[0].has_transform);
	BOOST_TEST(children[0].position == glm::vec3(4, 5, 6));
	BOOST_TEST(children[0].scale == glm::vec3(1, 1, 1));
	BOOST_TEST(!children[1].has_transform);
}

BOOST_AUTO_TEST_CASE(LayerV2_EditsOfWhatTheBaseNoLongerHas_AreIgnored)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a"}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"__order":["00000000000000ff","00000000000000b1"],"children":[
		{"__id":"00000000000000ee","__remove":true},
		{"__id":"00000000000000ed","name":"edit of a node that is gone"},
		{"__id":"00000000000000b1","components":{"ghost":null}}]})"));
	BOOST_REQUIRE(variant);
	BOOST_REQUIRE(variant->get_root().children.size() == 1u);
	BOOST_TEST(variant->get_root().children[0].name == "a");
}

BOOST_AUTO_TEST_CASE(LayerV2_ANodeMoved_IsARemoveAndANewNodeOfTheSameId)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","children":[{"__id":"00000000000000b2","name":"b"}]}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[
		{"__id":"00000000000000b1","children":[{"__id":"00000000000000b2","__remove":true}]},
		{"__id":"00000000000000b2","__new":true,"name":"b"}]})"));
	BOOST_REQUIRE(variant);
	const auto& root = variant->get_root();
	BOOST_REQUIRE(root.children.size() == 2u);
	BOOST_TEST(root.children[0].children.empty());
	BOOST_TEST(root.children[1].node_id == 0xb2ull);
}

BOOST_AUTO_TEST_CASE(LayerV2_ALayerWithoutAParentOrOfAnotherVersion_FailsTheLoad)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[]})");
	BOOST_TEST(!load<scn::prefab_desc>(put("no_parent.prefab.desc", R"({"__type":"prefab_desc","__version":4,"__layer":2})")));
	BOOST_TEST(!load<scn::prefab_desc>(put("future.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":3})")));
	BOOST_TEST(!load<scn::prefab_desc>(put("text.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":"2"})")));
}

BOOST_AUTO_TEST_CASE(LayerV2_ANullWhereOnlyAComponentOrATransformFieldMayBe_FailsTheLoad)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000b1","name":"a"}]})");
	BOOST_TEST(!load<scn::prefab_desc>(put("v1.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[{"__id":"00000000000000b1","name":null}]})")));
	BOOST_TEST(!load<scn::prefab_desc>(put("v2.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[{"__id":"00000000000000b1","overrides":null}]})")));
}

BOOST_AUTO_TEST_CASE(WriteLayer_AnEditedVariant_IsWrittenAsALayerAndReadsBack)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","components":{"stat":{"__type":"stat_desc","hp":1}}},
		{"__id":"00000000000000b2","name":"b"},
		{"__id":"00000000000000b3","name":"c"}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[]})"));
	BOOST_REQUIRE(variant);

	scn::prefab_desc edited;
	variant->copy_to(edited);
	edited.set_parent(variant->get_parent());
	auto& children = edited.get_root().children;
	children.erase(children.begin() + 1);
	std::swap(children[0], children[1]);
	children[0].name = "c_renamed";
	children[1].components.clear();

	json::object body;
	edited.serialize(body);
	BOOST_TEST(scn::write_prefab_layer(*descs, *rs, edited, body) == "");
	BOOST_TEST(body.at("__layer").as_int64() == 2);
	BOOST_TEST(body.at("__parent").as_string() == "memory://t/base.prefab.desc");

	scn::prefab_desc again;
	auto base = load<scn::prefab_desc>(tag_of("base.prefab.desc"));
	base->copy_to(again);
	again.set_parent(base->get_tag());
	again.deserialize(*descs, body);
	BOOST_TEST(json::value(saved(again)) == json::value(saved(edited)));
}

BOOST_AUTO_TEST_CASE(WriteLayer_AnInheritedOverrideRecordTakenOut_IsRefusedAndBodyStaysAsItWas)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","overrides":[{"target":["00000000000000b1"],"component":"stat","path":"/hp","value":5,"op":"set"}]}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2})"));
	BOOST_REQUIRE(variant);
	scn::prefab_desc edited;
	variant->copy_to(edited);
	edited.set_parent(variant->get_parent());
	edited.get_root().children[0].overrides.clear();

	json::object body;
	edited.serialize(body);
	const json::object before = body;
	BOOST_TEST(!scn::write_prefab_layer(*descs, *rs, edited, body).empty());
	BOOST_TEST(json::value(body) == json::value(before));
}

BOOST_AUTO_TEST_CASE(WriteLayer_RandomEdits_ReadBackAndWriteTheSameBytesAgain)
{
	for (uint64_t seed = 1; seed <= 60; ++seed) {
		splitmix64 rng{ seed };
		uint64_t next_id = 0x100;
		json::object base_file;
		base_file["__type"] = "prefab_desc";
		base_file["__version"] = 4;
		base_file["children"] = json::array{ random_node(rng, next_id, 1), random_node(rng, next_id, 1) };
		const std::string base_name = "base" + std::to_string(seed) + ".prefab.desc";
		put(base_name, json::serialize(base_file));
		auto base = load<scn::prefab_desc>(tag_of(base_name));
		BOOST_REQUIRE_MESSAGE(base, "seed " << seed);

		scn::prefab_desc edited;
		base->copy_to(edited);
		edited.set_parent(base->get_tag());
		mutate(rng, edited, next_id);

		json::object body;
		edited.serialize(body);
		const std::string why = scn::write_prefab_layer(*descs, *rs, edited, body);
		BOOST_REQUIRE_MESSAGE(why.empty(), "seed " << seed << ": " << why);

		scn::prefab_desc again;
		base->copy_to(again);
		again.set_parent(base->get_tag());
		again.deserialize(*descs, body);
		BOOST_REQUIRE_MESSAGE(json::value(saved(again)) == json::value(saved(edited)), "seed " << seed << ": the layer does not read back");

		json::object second;
		again.serialize(second);
		const std::string why_again = scn::write_prefab_layer(*descs, *rs, again, second);
		BOOST_REQUIRE_MESSAGE(why_again.empty(), "seed " << seed << ": " << why_again);
		BOOST_REQUIRE_MESSAGE(json::serialize(second) == json::serialize(body), "seed " << seed << ": the second write differs");
	}
}

BOOST_AUTO_TEST_CASE(WriteLayer_AChainOfTwoLayers_EachReadsBackOverItsOwnBase)
{
	for (uint64_t seed = 1; seed <= 30; ++seed) {
		splitmix64 rng{ seed * 7919 };
		uint64_t next_id = 0x100;
		json::object base_file;
		base_file["__type"] = "prefab_desc";
		base_file["__version"] = 4;
		base_file["children"] = json::array{ random_node(rng, next_id, 1), random_node(rng, next_id, 1) };
		const std::string base_name = "chain_base" + std::to_string(seed) + ".prefab.desc";
		put(base_name, json::serialize(base_file));
		auto base = load<scn::prefab_desc>(tag_of(base_name));
		BOOST_REQUIRE_MESSAGE(base, "seed " << seed);

		scn::prefab_desc first;
		base->copy_to(first);
		first.set_parent(base->get_tag());
		mutate(rng, first, next_id);
		json::object first_body;
		first.serialize(first_body);
		BOOST_REQUIRE_MESSAGE(scn::write_prefab_layer(*descs, *rs, first, first_body).empty(), "seed " << seed);
		const std::string first_name = "chain_first" + std::to_string(seed) + ".prefab.desc";
		first_body["__type"] = "prefab_desc";
		put(first_name, json::serialize(first_body));
		auto loaded_first = load<scn::prefab_desc>(tag_of(first_name));
		BOOST_REQUIRE_MESSAGE(loaded_first, "seed " << seed << ": the first layer does not load");
		BOOST_REQUIRE_MESSAGE(json::value(saved(*loaded_first)) == json::value(saved(first)), "seed " << seed << ": the first layer reads differently");

		scn::prefab_desc second;
		loaded_first->copy_to(second);
		second.set_parent(loaded_first->get_tag());
		mutate(rng, second, next_id);
		json::object second_body;
		second.serialize(second_body);
		const std::string why = scn::write_prefab_layer(*descs, *rs, second, second_body);
		BOOST_REQUIRE_MESSAGE(why.empty(), "seed " << seed << ": " << why);
		const std::string second_name = "chain_second" + std::to_string(seed) + ".prefab.desc";
		second_body["__type"] = "prefab_desc";
		put(second_name, json::serialize(second_body));
		auto loaded_second = load<scn::prefab_desc>(tag_of(second_name));
		BOOST_REQUIRE_MESSAGE(loaded_second, "seed " << seed << ": the second layer does not load");
		BOOST_REQUIRE_MESSAGE(json::value(saved(*loaded_second)) == json::value(saved(second)), "seed " << seed << ": the chain reads differently");
	}
}

BOOST_AUTO_TEST_CASE(WriteLayer_AVariantOfFormatV1_IsReadAsBeforeAndWrittenAsALayerThatReadsTheSame)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","components":{"stat":{"__type":"stat_desc","hp":1}}},
		{"__id":"00000000000000b2","name":"b"}]})");
	auto v1 = load<scn::prefab_desc>(put("v1.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","children":[
		{"__id":"00000000000000b1","name":"a_renamed","components":{"stat":{"__type":"stat_desc","hp":7}}},
		{"__id":"00000000000000c1","name":"c"}]})"));
	BOOST_REQUIRE(v1);
	BOOST_TEST(v1->get_root().children[0].name == "a_renamed");
	BOOST_TEST(v1->get_root().children.size() == 3u);

	json::object body;
	v1->serialize(body);
	BOOST_REQUIRE(scn::write_prefab_layer(*descs, *rs, *v1, body).empty());
	BOOST_TEST(body.at("__layer").as_int64() == 2);
	body["__type"] = "prefab_desc";
	auto v2 = load<scn::prefab_desc>(put("v2.prefab.desc", json::serialize(body)));
	BOOST_REQUIRE(v2);
	BOOST_TEST(json::value(saved(*v2)) == json::value(saved(*v1)));
}

BOOST_AUTO_TEST_CASE(WriteLayer_TheGenericWritePath_KeepsFormatV2_AndRefusesWhatNoLayerSays)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","overrides":[{"target":["00000000000000b1"],"component":"stat","path":"/hp","value":5,"op":"set"}]},
		{"__id":"00000000000000b2","name":"b"}]})");
	auto variant = load<scn::prefab_desc>(put("variant.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"__order":["00000000000000b2","00000000000000b1"],"children":[
		{"__id":"00000000000000b2","name":"b_renamed"}]})"));
	BOOST_REQUIRE(variant);

	json::object body;
	variant->serialize(body);
	descs->reduce_to_parent(*variant, body);
	BOOST_TEST(body.at("__layer").as_int64() == 2);
	body["__type"] = "prefab_desc";
	auto again = load<scn::prefab_desc>(put("again.prefab.desc", json::serialize(body)));
	BOOST_REQUIRE(again);
	json::object second;
	again->serialize(second);
	descs->reduce_to_parent(*again, second);
	second["__type"] = "prefab_desc";
	BOOST_TEST(json::serialize(second) == json::serialize(body));

	scn::prefab_desc edited;
	variant->copy_to(edited);
	edited.set_parent(variant->get_parent());
	edited.get_root().children[1].overrides.clear();
	json::object refused;
	edited.serialize(refused);
	BOOST_CHECK_THROW(descs->reduce_to_parent(edited, refused), std::exception);
}

BOOST_AUTO_TEST_CASE(LayerV2_AWorldWithALayer_FailsTheLoad)
{
	put("base.world.desc", R"({"__type":"world_desc","__version":4,"name":"w","systems":[],"children":[]})");
	BOOST_TEST(!load<scn::world_desc>(put("child.world.desc", R"({"__type":"world_desc","__version":4,
		"__parent":"memory://t/base.world.desc","__layer":2})")));
}

BOOST_AUTO_TEST_CASE(LayerV2_ATypoOrAnUnknownKeyInAnEntry_FailsTheLoadWithThePath)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000b1","name":"a"}]})");
	for (const char* entry : {
		R"({"__id":"00000000000000b1","compnents":{}})",
		R"({"__id":"00000000000000b1","__rmove":true})",
		R"({"__id":"00000000000000b1","__remove":true,"name":"x"})",
		R"({"__id":"00000000000000b1","__remove":true,"__new":true})",
	}) {
		const std::string text = std::string{ R"({"__type":"prefab_desc","__version":4,"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[)" } + entry + "]}";
		BOOST_TEST(!load<scn::prefab_desc>(put("bad" + std::to_string(std::hash<std::string>{}(entry)) + ".prefab.desc", text)), entry);
	}
}

BOOST_AUTO_TEST_CASE(LayerV2_ANewNodeIsReadStrictly_NoNullAndNoUnknownKey)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[]})");
	BOOST_TEST(!load<scn::prefab_desc>(put("n1.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[
		{"__id":"00000000000000c1","__new":true,"name":"c","components":{"stat":null}}]})")));
	BOOST_TEST(!load<scn::prefab_desc>(put("n2.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[
		{"__id":"00000000000000c1","__new":true,"name":"c","children":[{"__id":"00000000000000c2","nmae":"d"}]}]})")));
}

BOOST_AUTO_TEST_CASE(LayerV2_ANodeNamedTwiceInOneList_FailsTheLoad_OnlyAMoveIsAllowed)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000b1","name":"a"}]})");
	const auto text = [](const char* entries) {
		return std::string{ R"({"__type":"prefab_desc","__version":4,"__parent":"memory://t/base.prefab.desc","__layer":2,"children":[)" } + entries + "]}";
	};
	BOOST_TEST(!load<scn::prefab_desc>(put("t1.prefab.desc", text(R"({"__id":"00000000000000b1","__new":true},{"__id":"00000000000000b1","__remove":true})"))));
	BOOST_TEST(!load<scn::prefab_desc>(put("t2.prefab.desc", text(R"({"__id":"00000000000000b1","name":"x"},{"__id":"00000000000000b1","name":"y"})"))));
	BOOST_TEST(load<scn::prefab_desc>(put("t3.prefab.desc", text(R"({"__id":"00000000000000b1","__remove":true},{"__id":"00000000000000b1","__new":true,"name":"moved"})"))) != nullptr);
}

BOOST_AUTO_TEST_CASE(WriteLayer_AV1VariantThatHidAComponentOfTheBase_IsWrittenWithTheComponentRemoved)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"a","components":{"stat":{"__type":"stat_desc","hp":1}}}]})");
	auto v1 = load<scn::prefab_desc>(put("v1.prefab.desc", R"({"__type":"prefab_desc","__version":4,
		"__parent":"memory://t/base.prefab.desc","children":[{"__id":"00000000000000b1","name":"a"}]})"));
	BOOST_REQUIRE(v1);
	BOOST_TEST(v1->get_root().children[0].components.empty());
	json::object body;
	v1->serialize(body);
	BOOST_REQUIRE(scn::write_prefab_layer(*descs, *rs, *v1, body).empty());
	BOOST_TEST(json::serialize(body).find("\"stat\":null") != std::string::npos);
	body["__type"] = "prefab_desc";
	auto v2 = load<scn::prefab_desc>(put("v2.prefab.desc", json::serialize(body)));
	BOOST_REQUIRE(v2);
	BOOST_TEST(v2->get_root().children[0].components.empty());
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

BOOST_AUTO_TEST_CASE(PrettyWriter_LongStringListsGetALinePerElementAndEmptyObjectsStayCompact)
{
	json::value v = json::parse(R"({"short":["a","b"],"long":["scn::animation_controller_update","scn::animation_system_node",
		"scn::depth_system"],"empty":{}})");
	std::ostringstream ss;
	descs->write_pretty_json(ss, v);
	const auto text = ss.str();
	BOOST_TEST(text.find("[\"a\", \"b\"]") != std::string::npos);
	BOOST_TEST(text.find("\"empty\" : {}") != std::string::npos);
	BOOST_TEST(text.find("[\"scn::animation_controller_update\"") == std::string::npos);
	BOOST_TEST(json::value(json::parse(text)) == v);
}

BOOST_AUTO_TEST_CASE(PrettyWriter_DoublesStayDoublesAndReadBackExactly)
{
	json::value v = json::parse(R"({"a":[0.0,-1.0,0.9606748819351196,1e20,-0.0,2.5e-7],"n":3,"u":18446744073709551615})");
	std::ostringstream ss;
	descs->write_pretty_json(ss, v);
	const auto text = ss.str();
	BOOST_TEST(text.find("0.9606748819351196") != std::string::npos);
	BOOST_TEST(text.find("[0.0, -1.0,") != std::string::npos);
	auto back = json::parse(text);
	BOOST_TEST(back == v);
	BOOST_TEST(back.at("a").as_array()[0].is_double());
	BOOST_TEST(back.at("n").is_int64());
}

BOOST_AUTO_TEST_CASE(StampTree_StampsNestedTypedObjectsAndOrdersTheHeader)
{
	json::object obj = json::parse(R"({"children":[],"__guid":"g","name":"n","__type":"world_desc",
		"inner":[{"x":{"__type":"prefab_desc"}}, {"__type":"camera_desc"}]})").as_object();
	descs->stamp_tree(obj);

	std::vector<std::string> keys;
	for (auto& kv : obj) keys.emplace_back(kv.key());
	BOOST_TEST((keys == std::vector<std::string>{ "__type", "__version", "__guid", "children", "name", "inner" }));
	BOOST_TEST(obj.at("__version").as_int64() == 4);
	BOOST_TEST(obj.at("inner").as_array()[0].at("x").at("__version").as_int64() == 4);
	BOOST_TEST(obj.at("inner").as_array()[1].at("__version").as_int64() == 2);
}

BOOST_AUTO_TEST_SUITE_END()
