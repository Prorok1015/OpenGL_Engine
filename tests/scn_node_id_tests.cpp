#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#if SNK_HAS_ASSIMP
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#endif
#include <snk/desc/desc_node_id.h>
#include <snk/scene/level/scn_node_index.h>
#include "edt_scene_editor.h"
#include "edt_override_edit.h"
#include "edt_scene_document.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <set>

// Persistent node ids (__id) on prefab nodes and the node_id -> entity index of a spawned world.

namespace json = boost::json;

namespace {

using prefab_node = scn::prefab_desc::prefab_node;

constexpr const char* TREE_WITHOUT_IDS = R"({
	"__type": "prefab_desc",
	"children": {
		"arm": { "components": { "stat": { "__type": "stat_desc", "hp": 3 } },
		         "children": { "hand": { "components": { "stat": { "__type": "stat_desc", "hp": 1 } } } } },
		"leg": { "components": { "stat": { "__type": "stat_desc", "hp": 4 } } },
		"Light": { "__type": "directional_light_desc" }
	}
})";

constexpr const char* TREE_WITH_IDS = R"({
	"__type": "prefab_desc",
	"__id": "00000000000000aa",
	"children": {
		"arm": { "__id": "00000000000000bb", "components": { "stat": { "__type": "stat_desc", "hp": 3 } },
		         "children": { "hand": { "__id": "00000000000000cc" } } },
		"leg": { "__id": "00000000000000dd" }
	}
})";

#if SNK_HAS_ASSIMP
// glTF: node "Dup" (carrying a mesh) appears twice under "P"; Assimp keeps both names.
constexpr const char* GLTF_SAME_NAMES = R"GLTF({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"P","children":[1,2]},{"name":"Dup","mesh":0,"translation":[1,0,0]},{"name":"Dup","mesh":0,"translation":[2,0,0],"children":[3]},{"name":"Leaf"}],"meshes":[{"name":"TriMesh","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],"materials":[{"name":"Red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],"animations":[{"name":"Move","samplers":[{"input":4,"output":5,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}],"buffers":[{"byteLength":136,"uri":"data:application/octet-stream;base64,AACAvwAAgL8AAAAAAACAPwAAgL8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAD8AAIA/AAABAAIAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAIEEAAAAAAAAAAA=="}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6},{"buffer":0,"byteOffset":104,"byteLength":8},{"buffer":0,"byteOffset":112,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]},{"bufferView":5,"componentType":5126,"count":2,"type":"VEC3"}]})GLTF";

constexpr const char* OBJ_TWO_SAME_NAME = R"(v -1 -1 0
v 1 -1 0
v 0 1 0
v 2 2 0
v 3 2 0
v 2 3 0
g Tri
f 1 2 3
g Tri
f 4 5 6
)";
#endif

void collect_ids(const prefab_node& node, std::vector<uint64_t>& out)
{
	out.push_back(node.node_id);
	for (const auto& child : node.children) collect_ids(child, out);
}

std::vector<uint64_t> ids_of(const scn::prefab_desc& d)
{
	std::vector<uint64_t> out;
	collect_ids(d.get_root(), out);
	return out;
}

#if SNK_HAS_ASSIMP
void collect_names(const prefab_node& node, std::vector<std::string>& out)
{
	out.push_back(node.name);
	for (const auto& child : node.children) collect_names(child, out);
}
#endif

struct node_id_fixture : test_util::scene_env
{
#if SNK_HAS_ASSIMP
	node_id_fixture()
	{
		rs->registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(*descs), std::ref(*rs));
	}
#endif

	scn::prefab_desc parse(const char* text)
	{
		scn::prefab_desc d;
		d.deserialize(*descs, parse_migrated(text));
		pump();
		return d;
	}

	json::object serialized(const scn::prefab_desc& d)
	{
		json::object o;
		d.serialize(o);
		return o;
	}

#if SNK_HAS_ASSIMP
	std::shared_ptr<scn::prefab_desc> import(const char* name, const char* text)
	{
		auto tag = res::tag{ std::string("memory://models/") + name };
		rs->store(tag, test_util::to_bytes(text));
		return require_loaded<scn::prefab_desc>(tag).get_sync();
	}
#endif
};

}

BOOST_FIXTURE_TEST_SUITE(ScnNodeIdTests, node_id_fixture)

// ---------------------------------------------------------------- helpers

BOOST_AUTO_TEST_CASE(NodeId_HexFormatRoundTripsAndRejectsGarbage)
{
	BOOST_TEST(desc::format_node_id(0xabcdef) == "0000000000abcdef");
	BOOST_TEST(desc::parse_node_id("0000000000abcdef").value() == 0xabcdefull);
	BOOST_TEST(!desc::parse_node_id("abcdef").has_value());               // wrong length
	BOOST_TEST(!desc::parse_node_id("000000000000000g").has_value());     // not hex
	BOOST_TEST(!desc::parse_node_id("0000000000000000").has_value());     // 0 is invalid
	for (int i = 0; i < 1000; ++i) BOOST_REQUIRE(desc::new_node_id() != desc::invalid_node_id);
}

// ---------------------------------------------------------------- load / save

BOOST_AUTO_TEST_CASE(Load_ReadsExplicitIdsAndSaveWritesThemAsHexStrings)
{
	auto d = parse(TREE_WITH_IDS);
	BOOST_TEST(d.get_root().node_id == 0xaaull);
	BOOST_TEST(d.get_root().children[0].node_id == 0xbbull);
	BOOST_TEST(d.get_root().children[0].children[0].node_id == 0xccull);

	auto out = serialized(d);
	BOOST_TEST(out.at("__id").as_string() == "00000000000000aa");
	const auto& arm = out.at("children").as_array().at(0);
	BOOST_TEST(arm.at("name").as_string() == "arm");
	BOOST_TEST(arm.at("__id").as_string() == "00000000000000bb");
	BOOST_TEST(arm.at("children").as_array().at(0).at("__id").as_string() == "00000000000000cc");
}

BOOST_AUTO_TEST_CASE(RoundTrip_LoadSaveLoadKeepsIdsAndSaveIsAFixedPoint)
{
	auto first = parse(TREE_WITHOUT_IDS);
	auto saved = serialized(first);
	scn::prefab_desc second;
	second.deserialize(*descs, saved);
	pump();

	BOOST_TEST((ids_of(first) == ids_of(second)));
	BOOST_TEST(json::value(saved) == json::value(serialized(second)));
}

BOOST_AUTO_TEST_CASE(Load_FileWithoutIdsDerivesStableNonZeroDistinctIds)
{
	auto a = parse(TREE_WITHOUT_IDS);
	auto b = parse(TREE_WITHOUT_IDS);
	auto ids = ids_of(a);

	BOOST_TEST((ids == ids_of(b)));
	BOOST_TEST(std::ranges::none_of(ids, [](uint64_t id) { return id == desc::invalid_node_id; }));
	BOOST_TEST(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
	BOOST_TEST(a.get_root().children[1].node_id == desc::derive_node_id("/leg"));
	BOOST_TEST(a.get_root().children[0].children[0].node_id == desc::derive_node_id("/arm/hand"));
}

BOOST_AUTO_TEST_CASE(Load_SameFileFromResourceSystemTwiceGivesIdenticalIds)
{
	auto a = load<scn::prefab_desc>(put("one.prefab.desc", TREE_WITHOUT_IDS));
	auto b = load<scn::prefab_desc>(put("two.prefab.desc", TREE_WITHOUT_IDS));
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(b);
	BOOST_TEST((ids_of(*a) == ids_of(*b)));
}

BOOST_AUTO_TEST_CASE(Load_DuplicateIdRegeneratesTheSecondNode)
{
	auto d = parse(R"({
		"children": {
			"a": { "__id": "00000000000000aa" },
			"b": { "__id": "00000000000000aa", "children": { "c": { "__id": "00000000000000aa" } } }
		}
	})");
	const auto& a = d.get_root().children[0];
	const auto& b = d.get_root().children[1];
	const auto& c = b.children[0];

	BOOST_TEST(a.node_id == 0xaaull);
	BOOST_TEST(b.node_id != 0xaaull);
	BOOST_TEST(c.node_id != 0xaaull);
	BOOST_TEST(b.node_id != c.node_id);
	BOOST_TEST(b.node_id != desc::invalid_node_id);
	BOOST_TEST(c.node_id != desc::invalid_node_id);
}

BOOST_AUTO_TEST_CASE(Load_MalformedIdFallsBackToDerived)
{
	auto d = parse(R"({"children": { "a": { "__id": "xyz" }, "b": { "__id": 17 } }})");
	BOOST_TEST(d.get_root().children[0].node_id == desc::derive_node_id("/a"));
	BOOST_TEST(d.get_root().children[1].node_id == desc::derive_node_id("/b"));
}

BOOST_AUTO_TEST_CASE(Load_ShorthandNodeKeepsItsIdAcrossSave)
{
	auto first = parse(R"({"children": { "Light": { "__type": "directional_light_desc" } }})");
	auto id = first.get_root().children[0].node_id;
	BOOST_TEST(id != desc::invalid_node_id);

	scn::prefab_desc second;
	second.deserialize(*descs, serialized(first));
	pump();
	BOOST_TEST(second.get_root().children[0].node_id == id);
	BOOST_TEST(second.get_root().children[0].components.contains("Light"));
}

BOOST_AUTO_TEST_CASE(Load_MetadataKeysAreNotComponentOverrides)
{
	auto d = parse(R"({"children": { "n": { "__id": "00000000000000aa", "components": {
		"stat": { "__type": "stat_desc", "__version": 1, "__id": "00000000000000bb", "hp": 5 } } } }})");
	const auto& overrides = d.get_root().children[0].components.at("stat").overrides;
	BOOST_TEST(overrides.size() == 1u);
	BOOST_TEST(overrides.contains("hp"));
}

BOOST_AUTO_TEST_CASE(Load_ParentLayerKeepsParentIdsAndDerivesNewNodes)
{
	put("base.prefab.desc", TREE_WITH_IDS);
	// Same layering the adapter does: copy of the parent, then the overlay deserialized on top.
	auto base = load<scn::prefab_desc>(res::tag{ "memory://t/base.prefab.desc" });
	BOOST_REQUIRE(base);
	scn::prefab_desc child = *base;
	child.deserialize(*descs, json::parse(R"({"__parent": "memory://t/base.prefab.desc", "__version": 2,
		"children": [ { "name": "extra", "components": { "stat": { "__type": "stat_desc" } } } ] })").as_object());

	BOOST_TEST(child.get_root().node_id == 0xaaull);
	BOOST_TEST(child.get_root().children[0].node_id == 0xbbull);
	BOOST_REQUIRE(child.get_root().children.size() == 3u);
	BOOST_TEST(child.get_root().children[2].node_id == desc::derive_node_id("/extra"));
}

// ---------------------------------------------------------------- editor

struct editor_fixture : node_id_fixture
{
	editor_fixture()
	{
		state.world_descs.emplace_back();
		state.world_names.push_back("w");
		world().deserialize(*descs, parse_migrated(TREE_WITH_IDS));
		editor = std::make_unique<edt::scene_editor>(state);
	}

	scn::world_desc& world() { return state.world_descs[0]; }

	edt::scene_document state;
	std::unique_ptr<edt::scene_editor> editor;
};

BOOST_FIXTURE_TEST_CASE(Editor_CreatedEntityGetsRandomId, editor_fixture)
{
	editor->create_entity("Empty");
	editor->create_entity("Empty");
	const auto& children = world().get_root().children;
	BOOST_REQUIRE(children.size() == 4u);
	BOOST_TEST(children[2].node_id != desc::invalid_node_id);
	BOOST_TEST(children[3].node_id != desc::invalid_node_id);
	BOOST_TEST(children[2].node_id != children[3].node_id);
}

BOOST_FIXTURE_TEST_CASE(Editor_SyncLeavesAnInstanceWithoutATransformAlone, node_id_fixture)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":3,"transform":{"position":[1,2,3]}})");
	scn::prefab_desc level;
	level.deserialize(*descs, parse_migrated(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","components":{"base":"memory://t/base.prefab.desc"}},
		{"__id":"00000000000000c2","name":"plain","components":{"stat":{"__type":"stat_desc"}}}]})"));
	json::object before;
	level.serialize(before);

	// the live entities sit where the prefab root put the instance
	glm::mat4 root_placement = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
	edt::scene_editor::write_live_transform(level.get_root().children[0], root_placement);
	edt::scene_editor::write_live_transform(level.get_root().children[1], glm::mat4(1.0f));

	json::object after;
	level.serialize(after);
	BOOST_TEST(json::value(before) == json::value(after));
	BOOST_TEST(!level.get_root().children[0].has_transform);
}

BOOST_FIXTURE_TEST_CASE(Editor_SyncWritesTheTransformOfAnInstanceTheUserPlaced, node_id_fixture)
{
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":3,"transform":{"position":[1,2,3]}})");
	scn::prefab_desc level;
	level.deserialize(*descs, parse_migrated(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","transform":{"position":[4,0,0]},"components":{"base":"memory://t/base.prefab.desc"}}]})"));
	edt::scene_editor::write_live_transform(level.get_root().children[0], glm::mat4(1.0f));
	const auto& node = level.get_root().children[0];
	BOOST_TEST((node.position == glm::vec3{ 0 }));
	BOOST_TEST(node.has_transform);
}

BOOST_FIXTURE_TEST_CASE(Parse_MalformedTransformValuesAreIgnoredWithoutThrowing, node_id_fixture)
{
	scn::prefab_desc level;
	BOOST_CHECK_NO_THROW(level.deserialize(*descs, parse_migrated(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"a","transform":{"position":["a","b","c"],"scale":[2,2]}}]})")));
	const auto& node = level.get_root().children[0];
	BOOST_TEST((node.position == glm::vec3{ 0 }));
	BOOST_TEST((node.scale == glm::vec3{ 1 }));
}

BOOST_FIXTURE_TEST_CASE(Editor_DuplicateGivesTheWholeSubtreeFreshIds, editor_fixture)
{
	editor->duplicate_entity(0xbbull);
	const auto& children = world().get_root().children;
	BOOST_REQUIRE(children.size() == 3u);

	const auto& original = children[0];
	const auto& copy = children[1];
	BOOST_TEST(copy.name == "arm_copy");
	BOOST_TEST(original.node_id == 0xbbull);
	BOOST_TEST(original.children[0].node_id == 0xccull);
	BOOST_TEST(copy.node_id != original.node_id);
	BOOST_TEST(copy.node_id != desc::invalid_node_id);
	BOOST_REQUIRE(copy.children.size() == 1u);
	BOOST_TEST(copy.children[0].node_id != original.children[0].node_id);
	BOOST_TEST(copy.children[0].node_id != desc::invalid_node_id);
}

BOOST_FIXTURE_TEST_CASE(Editor_DuplicateManyAndReparentKeepIds, editor_fixture)
{
	editor->duplicate_entities({ 0xbbull, 0xddull });
	std::vector<uint64_t> ids;
	collect_ids(world().get_root(), ids);
	BOOST_TEST(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());

	editor->reparent_entity(0xddull, 0xbbull, -1);
	const auto* leg = edt::scene_editor::find_node_by_id(world().get_root().children, 0xddull);
	BOOST_REQUIRE(leg);
	BOOST_TEST(leg->name == "leg");
	const auto* arm = edt::scene_editor::find_node_by_id(world().get_root().children, 0xbbull);
	BOOST_REQUIRE(arm);
	BOOST_TEST(std::ranges::count(arm->children, 0xddull, &prefab_node::node_id) == 1);
}

// Nodes are edited by id; instance_fixture has two nodes named "twin" and an instance "robot" of rig.prefab.desc
// whose reference component is keyed "model_key".
struct instance_fixture : node_id_fixture
{
	instance_fixture()
	{
		auto rig = put("rig.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
			{"__id":"00000000000000a1","name":"arm","transform":{"position":[5,0,0]},
				"components":{"stat":{"__type":"stat_desc","hp":3}}}]})");
		load<scn::prefab_desc>(rig); // resolves the reference handle of robot before it is parsed
		state.world_descs.emplace_back();
		state.world_names.push_back("w");
		world().deserialize(*descs, parse_migrated(R"({"__type":"prefab_desc","__version":3,"children":[
			{"__id":"00000000000000b1","name":"twin"},
			{"__id":"00000000000000b2","name":"twin"},
			{"__id":"00000000000000c1","name":"robot","components":{"model_key":"memory://t/rig.prefab.desc"}}]})"));
		pump(); // the parent_desc handles of the instances load in the background
		state.assembler = assembler.get();
		editor = std::make_unique<edt::scene_editor>(state);
	}

	scn::world_desc& world() { return state.world_descs[0]; }
	std::vector<prefab_node>& nodes() { return world().get_root().children; }

	edt::scene_document state;
	std::unique_ptr<edt::scene_editor> editor;
};

BOOST_FIXTURE_TEST_CASE(Editor_SameNamedNodesAreEditedIndependently, instance_fixture)
{
	const std::vector<uint64_t> second{ 0xb2ull };
	editor->on_transform_committed(second, glm::vec3{ 1, 2, 3 }, glm::vec3{ 0 }, glm::vec3{ 1 });
	BOOST_TEST(!nodes()[0].has_transform);
	BOOST_TEST(nodes()[1].has_transform);
	BOOST_TEST((nodes()[1].position == glm::vec3{ 1, 2, 3 }));

	editor->delete_entity(0xb1ull);
	BOOST_REQUIRE(nodes().size() == 2u);
	BOOST_TEST(nodes()[0].node_id == 0xb2ull);
	BOOST_TEST(nodes()[0].name == "twin");
}

BOOST_FIXTURE_TEST_CASE(Editor_CommittedTransformLandsOnTheNodeNotOnItsReferenceKey, instance_fixture)
{
	const std::vector<uint64_t> robot{ 0xc1ull };
	editor->on_transform_committed(robot, glm::vec3{ 4, 0, 0 }, glm::vec3{ 0 }, glm::vec3{ 1 });
	BOOST_TEST(nodes()[2].name == "robot");
	BOOST_TEST(nodes()[2].has_transform);
	BOOST_TEST((nodes()[2].position == glm::vec3{ 4, 0, 0 }));
	BOOST_TEST(nodes()[2].overrides.empty());
}

BOOST_FIXTURE_TEST_CASE(Editor_ResolveNodePathSplitsAtTheInstanceBoundary, instance_fixture)
{
	const std::vector<uint64_t> own{ 0xc1ull };
	auto direct = edt::scene_editor::resolve_node_path(nodes(), own);
	BOOST_TEST(direct.owner == &nodes()[2]);
	BOOST_TEST(direct.target.empty());

	const std::vector<uint64_t> inside{ 0xc1ull, 0xa1ull };
	auto below = edt::scene_editor::resolve_node_path(nodes(), inside);
	BOOST_TEST(below.owner == &nodes()[2]);
	BOOST_TEST((below.target == std::vector<uint64_t>{ 0xa1ull }));

	const std::vector<uint64_t> unknown{ 0x99ull };
	BOOST_TEST(edt::scene_editor::resolve_node_path(nodes(), unknown).owner == nullptr);
	BOOST_TEST(edt::scene_editor::resolve_node_path(nodes(), {}).owner == nullptr);
}

BOOST_FIXTURE_TEST_CASE(Editor_TransformInsideAnInstanceBecomesAnOverrideRecord, instance_fixture)
{
	const std::vector<uint64_t> path{ 0xc1ull, 0xa1ull };
	editor->on_transform_committed(path, glm::vec3{ 9, 0, 0 }, glm::vec3{ 0 }, glm::vec3{ 1 });

	auto& robot = nodes()[2];
	BOOST_TEST(!robot.has_transform);
	BOOST_REQUIRE(robot.overrides.size() == 1u);
	const auto& record = robot.overrides[0];
	BOOST_TEST((record.target == std::vector<uint64_t>{ 0xa1ull }));
	BOOST_TEST(record.component == "transform");
	BOOST_TEST(record.path.empty());
	BOOST_TEST((record.value == json::value{ {"position", {9.0, 0.0, 0.0}} }));

	// putting it back where the prefab has it is no override at all
	editor->on_transform_committed(path, glm::vec3{ 5, 0, 0 }, glm::vec3{ 0 }, glm::vec3{ 1 });
	BOOST_TEST(robot.overrides.empty());
}

BOOST_FIXTURE_TEST_CASE(Override_ComponentEditIsOneRecordAndResetRemovesIt, instance_fixture)
{
	namespace oe = edt::override_edit;
	auto& robot = nodes()[2];
	const std::vector<uint64_t> target{ 0xa1ull };
	const auto arm = oe::find_base_node(*assembler, robot, target);
	BOOST_REQUIRE(arm);
	BOOST_TEST(arm->name == "arm");

	const auto base_json = oe::component_json(arm->components.at("stat"));
	auto edited = base_json;
	edited["hp"] = 20;
	oe::write_component(robot, target, "stat", base_json, edited);

	BOOST_REQUIRE(robot.overrides.size() == 1u);
	BOOST_TEST(robot.overrides[0].component == "stat");
	BOOST_TEST((robot.overrides[0].value == json::value{ {"hp", 20} }));
	BOOST_TEST(oe::has_record(robot, target, "stat"));
	BOOST_TEST(oe::effective_component(robot, target, "stat", base_json).at("hp").to_number<int64_t>() == 20);

	// editing again replaces the record instead of stacking another one
	edited["hp"] = 30;
	oe::write_component(robot, target, "stat", base_json, edited);
	BOOST_REQUIRE(robot.overrides.size() == 1u);
	BOOST_TEST(oe::effective_component(robot, target, "stat", base_json).at("hp").to_number<int64_t>() == 30);

	BOOST_TEST(oe::reset(robot, target, "stat"));
	BOOST_TEST(robot.overrides.empty());
	BOOST_TEST(!oe::reset(robot, target, "stat"));
	BOOST_TEST(oe::effective_component(robot, target, "stat", base_json).at("hp").to_number<int64_t>() == 3);
}

BOOST_FIXTURE_TEST_CASE(Override_RecordsSurviveSaveAndLoad, instance_fixture)
{
	namespace oe = edt::override_edit;
	const std::vector<uint64_t> path{ 0xc1ull, 0xa1ull };
	editor->on_transform_committed(path, glm::vec3{ 9, 0, 0 }, glm::vec3{ 0 }, glm::vec3{ 1 });
	const std::vector<uint64_t> target{ 0xa1ull };
	const auto base_json = oe::component_json(oe::find_base_node(*assembler, nodes()[2], target)->components.at("stat"));
	auto edited = base_json;
	edited["hp"] = 20;
	oe::write_component(nodes()[2], target, "stat", base_json, edited);

	json::object saved;
	world().serialize(saved);
	scn::world_desc loaded;
	loaded.deserialize(*descs, saved);
	pump();

	const auto& robot = loaded.get_root().children[2];
	BOOST_TEST(robot.node_id == 0xc1ull);
	BOOST_REQUIRE(robot.overrides.size() == 2u);
	BOOST_TEST(std::ranges::count(robot.overrides, "transform", &scn::override_record::component) == 1);
	BOOST_TEST(std::ranges::count(robot.overrides, "stat", &scn::override_record::component) == 1);
	for (const auto& record : robot.overrides) {
		BOOST_TEST((record.target == std::vector<uint64_t>{ 0xa1ull }));
	}
}

BOOST_FIXTURE_TEST_CASE(Override_RemoveRecordsSurviveEditsAndResets, instance_fixture)
{
	namespace oe = edt::override_edit;
	auto& robot = nodes()[2];
	const std::vector<uint64_t> target{ 0xa1ull };
	scn::override_record removal;
	removal.target = target;
	removal.component = "stat";
	removal.op = scn::override_op::remove;
	robot.overrides.push_back(removal);

	BOOST_TEST(!oe::has_record(robot, target, "stat"));
	BOOST_TEST(!oe::reset(robot, target, "stat"));
	oe::write_component(robot, target, "stat", json::object{ {"hp", 3} }, json::object{ {"hp", 4} });
	BOOST_TEST(oe::reset(robot, target, "stat"));
	BOOST_TEST(oe::reset(robot, target, std::string{ scn::transform_pseudo_component }) == false);
	BOOST_REQUIRE(robot.overrides.size() == 1u);
	BOOST_TEST((robot.overrides[0].op == scn::override_op::remove));
}

BOOST_AUTO_TEST_CASE(Override_DiffComparesNumbersByValue)
{
	auto changed = edt::override_edit::diff(json::object{ {"hp", 1}, {"scale", 2.0} }, json::object{ {"hp", 1.0}, {"scale", 2} });
	BOOST_TEST(changed.empty());
	changed = edt::override_edit::diff(json::object{ {"hp", 1} }, json::object{ {"hp", 1.5} });
	BOOST_TEST(changed.size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(Override_EquivalentEulerRotationIsNoOverride, instance_fixture)
{
	namespace oe = edt::override_edit;
	prefab_node base;
	base.rotation = glm::vec3{ 0, 270, 0 };
	auto& robot = nodes()[2];
	oe::write_transform(robot, std::vector<uint64_t>{ 0xa1ull }, base, base.position, glm::vec3{ 0, -90, 0 }, base.scale);
	BOOST_TEST(robot.overrides.empty());
	oe::write_transform(robot, std::vector<uint64_t>{ 0xa1ull }, base, base.position, glm::vec3{ 0, 0, 0 }, base.scale);
	BOOST_TEST(robot.overrides.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Duplicate_RemapsRecordsAddressedToTheCopiedSubtree)
{
	prefab_node inst;
	inst.node_id = 0xc1ull;
	prefab_node literal;
	literal.node_id = 0xd1ull;
	inst.children.push_back(literal);

	scn::override_record on_literal;
	on_literal.target = { 0xd1ull };
	on_literal.component = "stat";
	on_literal.value = json::object{ {"hp", 2} };
	scn::override_record on_content;
	on_content.target = { 0xa1ull };
	on_content.component = "stat";
	scn::override_record added;
	added.op = scn::override_op::add_node;
	added.value = json::object{ {"__id", desc::format_node_id(0xe1ull)}, {"name", "extra"} };
	scn::override_record on_added;
	on_added.target = { 0xe1ull };
	on_added.component = "stat";
	inst.overrides = { on_literal, on_content, added, on_added };

	scn::regenerate_node_ids(inst);

	const uint64_t new_literal = inst.children[0].node_id;
	BOOST_TEST(new_literal != 0xd1ull);
	BOOST_TEST((inst.overrides[0].target == std::vector<uint64_t>{ new_literal }));
	BOOST_TEST((inst.overrides[1].target == std::vector<uint64_t>{ 0xa1ull }));
	const auto new_added = desc::parse_node_id(inst.overrides[2].value.as_object().at("__id").as_string());
	BOOST_REQUIRE(new_added);
	BOOST_TEST(*new_added != 0xe1ull);
	BOOST_TEST((inst.overrides[3].target == std::vector<uint64_t>{ *new_added }));
}

BOOST_FIXTURE_TEST_CASE(Override_TransformIsEffectiveAndResettable, instance_fixture)
{
	namespace oe = edt::override_edit;
	auto& robot = nodes()[2];
	const std::vector<uint64_t> target{ 0xa1ull };
	const auto arm = oe::find_base_node(*assembler, robot, target);
	BOOST_REQUIRE(arm);

	oe::write_transform(robot, target, *arm, glm::vec3{ 1, 1, 1 }, arm->rotation, arm->scale);
	glm::vec3 pos, rot, scl;
	oe::effective_transform(robot, target, *arm, pos, rot, scl);
	BOOST_TEST((pos == glm::vec3{ 1, 1, 1 }));
	BOOST_TEST((scl == arm->scale));

	BOOST_TEST(oe::reset(robot, target, std::string{ scn::transform_pseudo_component }));
	oe::effective_transform(robot, target, *arm, pos, rot, scl);
	BOOST_TEST((pos == glm::vec3{ 5, 0, 0 }));
}

// Two instance levels: the level's "robot" instantiates body.prefab.desc, whose node "mid" instantiates
// gear.prefab.desc and carries records on gear's nodes (set, remove, add_node). Edits of the level go on top of those.
struct nested_fixture : node_id_fixture
{
	nested_fixture()
	{
		load<scn::prefab_desc>(put("gear.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
			{"__id":"00000000000000a1","name":"tooth","transform":{"position":[1,0,0]},
				"components":{"stat":{"__type":"stat_desc","hp":3}}},
			{"__id":"00000000000000a2","name":"tail","components":{"stat":{"__type":"stat_desc","hp":4}}},
			{"__id":"00000000000000a3","name":"wing"}]})"));
		load<scn::prefab_desc>(put("body.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
			{"__id":"00000000000000b1","name":"mid","components":{"gear_key":"memory://t/gear.prefab.desc"},"overrides":[
				{"target":["00000000000000a1"],"component":"stat","path":"","value":{"hp":7}},
				{"target":["00000000000000a1"],"component":"transform","path":"","value":{"position":[2,0,0]}},
				{"op":"remove","target":["00000000000000a2"],"component":"stat"},
				{"op":"remove","target":["00000000000000a3"]},
				{"op":"add_node","target":[],"value":{"__id":"00000000000000e1","name":"extra",
					"components":{"stat":{"__type":"stat_desc","hp":9}}}}]}]})"));
		state.world_descs.emplace_back();
		state.world_names.push_back("w");
		world().deserialize(*descs, parse_migrated(R"({"__type":"prefab_desc","__version":3,"children":[
			{"__id":"00000000000000c1","name":"robot","components":{"model_key":"memory://t/body.prefab.desc"}}]})"));
		pump(); // the parent_desc handles of the instances load in the background
		state.assembler = assembler.get();
	}

	scn::world_desc& world() { return state.world_descs[0]; }
	prefab_node& robot() { return world().get_root().children[0]; }

	edt::scene_document state;
};

BOOST_FIXTURE_TEST_CASE(Override_BaseTwoLevelsDeepCarriesTheIntermediateRecords, nested_fixture)
{
	namespace oe = edt::override_edit;
	const std::vector<uint64_t> tooth{ 0xb1ull, 0xa1ull };
	const auto base = oe::find_base_node(*assembler, robot(), tooth);
	BOOST_REQUIRE(base);
	BOOST_TEST(base->name == "tooth");
	BOOST_TEST((base->position == glm::vec3{ 2, 0, 0 }));          // body's transform record, not gear's raw {1,0,0}
	const auto base_json = oe::component_json(base->components.at("stat"));
	BOOST_TEST(base_json.at("hp").to_number<int64_t>() == 7);        // body's set record, not gear's raw 3
	BOOST_TEST(oe::effective_component(robot(), tooth, "stat", base_json).at("hp").to_number<int64_t>() == 7);
}

BOOST_FIXTURE_TEST_CASE(Override_EditBackToTheRawValueIsStillAnOverrideOfTheComposedBase, nested_fixture)
{
	namespace oe = edt::override_edit;
	const std::vector<uint64_t> tooth{ 0xb1ull, 0xa1ull };
	const auto base_json = oe::component_json(oe::find_base_node(*assembler, robot(), tooth)->components.at("stat"));

	auto edited = base_json;
	edited["hp"] = 3; // gear's own value, but the instance has 7 from body
	oe::write_component(robot(), tooth, "stat", base_json, edited);
	BOOST_REQUIRE(robot().overrides.size() == 1u);
	BOOST_TEST((robot().overrides[0].value == json::value{ {"hp", 3} }));
	BOOST_TEST(oe::effective_component(robot(), tooth, "stat", base_json).at("hp").to_number<int64_t>() == 3);

	// the base does not move with the level's own record
	const auto again = oe::component_json(oe::find_base_node(*assembler, robot(), tooth)->components.at("stat"));
	BOOST_TEST(again.at("hp").to_number<int64_t>() == 7);

	// editing back to the composed base drops the record, "Reset to base" restores body's value
	edited["hp"] = 7;
	oe::write_component(robot(), tooth, "stat", again, edited);
	BOOST_TEST(robot().overrides.empty());
	edited["hp"] = 3;
	oe::write_component(robot(), tooth, "stat", again, edited);
	BOOST_TEST(oe::reset(robot(), tooth, "stat"));
	BOOST_TEST(oe::effective_component(robot(), tooth, "stat", again).at("hp").to_number<int64_t>() == 7);
}

BOOST_FIXTURE_TEST_CASE(Override_TransformIsRelativeToTheComposedBase, nested_fixture)
{
	namespace oe = edt::override_edit;
	const std::vector<uint64_t> tooth{ 0xb1ull, 0xa1ull };
	const auto base = oe::find_base_node(*assembler, robot(), tooth);
	BOOST_REQUIRE(base);

	oe::write_transform(robot(), tooth, *base, base->position, base->rotation, base->scale);
	BOOST_TEST(robot().overrides.empty()); // the composed position is no override

	oe::write_transform(robot(), tooth, *base, glm::vec3{ 1, 0, 0 }, base->rotation, base->scale);
	BOOST_REQUIRE(robot().overrides.size() == 1u);
	glm::vec3 pos, rot, scl;
	oe::effective_transform(robot(), tooth, *base, pos, rot, scl);
	BOOST_TEST((pos == glm::vec3{ 1, 0, 0 }));

	BOOST_TEST(oe::reset(robot(), tooth, std::string{ scn::transform_pseudo_component }));
	oe::effective_transform(robot(), tooth, *oe::find_base_node(*assembler, robot(), tooth), pos, rot, scl);
	BOOST_TEST((pos == glm::vec3{ 2, 0, 0 }));
}

BOOST_FIXTURE_TEST_CASE(Override_BaseAppliesRemoveAndAddNodeRecordsOfIntermediateLevels, nested_fixture)
{
	namespace oe = edt::override_edit;
	const auto tail = oe::find_base_node(*assembler, robot(), std::vector<uint64_t>{ 0xb1ull, 0xa2ull });
	BOOST_REQUIRE(tail);
	BOOST_TEST(tail->components.count("stat") == 0u); // body removed the component

	BOOST_TEST(!oe::find_base_node(*assembler, robot(), std::vector<uint64_t>{ 0xb1ull, 0xa3ull }).has_value()); // body removed the node

	const auto extra = oe::find_base_node(*assembler, robot(), std::vector<uint64_t>{ 0xb1ull, 0xe1ull });
	BOOST_REQUIRE(extra); // body added the node
	BOOST_TEST(extra->name == "extra");
	BOOST_TEST(oe::component_json(extra->components.at("stat")).at("hp").to_number<int64_t>() == 9);

	BOOST_TEST(!oe::find_base_node(*assembler, robot(), std::vector<uint64_t>{ 0xb1ull, 0x99ull }).has_value());
}

BOOST_FIXTURE_TEST_CASE(Override_ListedChildrenAreTheComposedNodes, nested_fixture)
{
	namespace oe = edt::override_edit;
	const auto top = oe::list_children(*assembler, robot(), std::span<const uint64_t>{});
	BOOST_REQUIRE(top.size() == 1u);
	BOOST_TEST(top[0].node.node_id == 0xb1ull);
	BOOST_TEST(top[0].has_children); // mid instantiates gear

	const auto below = oe::list_children(*assembler, robot(), std::vector<uint64_t>{ 0xb1ull });
	std::vector<uint64_t> ids;
	for (const auto& listed : below) ids.push_back(listed.node.node_id);
	// body removed wing and added extra; the inspector's base has the same nodes
	BOOST_TEST((ids == std::vector<uint64_t>{ 0xa1ull, 0xa2ull, 0xe1ull }));
}

BOOST_FIXTURE_TEST_CASE(Override_ViewCacheComposesOncePerGeneration, nested_fixture)
{
	namespace oe = edt::override_edit;
	const std::vector<uint64_t> tooth{ 0xb1ull, 0xa1ull };
	const std::vector<uint64_t> mid{ 0xb1ull };
	oe::view_cache cache(*assembler, state);

	const auto first = cache.base(robot(), tooth);
	BOOST_REQUIRE(first && *first);
	BOOST_TEST(cache.base(robot(), tooth) == first); // a second frame reuses the composed base
	const auto listing = cache.children(robot(), mid);
	BOOST_TEST(cache.children(robot(), mid) == listing);

	// a desc change moves the generation: the next ask composes again
	state.touch_descs();
	const auto second = cache.base(robot(), tooth);
	BOOST_TEST(second != first);
	BOOST_REQUIRE(second && *second);
	BOOST_TEST(oe::component_json((*second)->components.at("stat")).at("hp").to_number<int64_t>() == 7);
	BOOST_TEST(cache.children(robot(), mid) != listing);
}

BOOST_FIXTURE_TEST_CASE(Override_ViewCacheResultsOutliveInvalidation, nested_fixture)
{
	namespace oe = edt::override_edit;
	const std::vector<uint64_t> tooth{ 0xb1ull, 0xa1ull };
	const std::vector<uint64_t> mid{ 0xb1ull };
	oe::view_cache cache(*assembler, state);

	const auto base = cache.base(robot(), tooth);
	const auto listing = cache.children(robot(), mid);
	cache.invalidate();
	state.touch_descs();
	cache.base(robot(), tooth); // recomposing must not free what is held
	cache.children(robot(), mid);

	BOOST_REQUIRE(base && *base);
	BOOST_TEST((*base)->name == "tooth");
	BOOST_REQUIRE(listing);
	BOOST_TEST(listing->size() == 3u);
}

BOOST_FIXTURE_TEST_CASE(Editor_TransformCommitInsideANestedInstanceIsRelativeToTheComposedBase, nested_fixture)
{
	const std::vector<uint64_t> path{ 0xc1ull, 0xb1ull, 0xa1ull };
	edt::scene_editor editor_under_test(state);
	editor_under_test.on_transform_committed(path, glm::vec3{ 2, 0, 0 }, glm::vec3{ 0 }, glm::vec3{ 1 });
	BOOST_TEST(robot().overrides.empty()); // body already puts it at {2,0,0}
	editor_under_test.on_transform_committed(path, glm::vec3{ 5, 0, 0 }, glm::vec3{ 0 }, glm::vec3{ 1 });
	BOOST_REQUIRE(robot().overrides.size() == 1u);
	BOOST_TEST((robot().overrides[0].target == std::vector<uint64_t>{ 0xb1ull, 0xa1ull }));
}

// ---------------------------------------------------------------- importer

#if SNK_HAS_ASSIMP

BOOST_AUTO_TEST_CASE(Importer_IdsAreDeterministicAcrossImports)
{
	auto a = import("d1/x.obj", OBJ_TWO_SAME_NAME);
	auto b = import("d2/x.obj", OBJ_TWO_SAME_NAME);
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(b);
	auto ids = ids_of(*a);
	BOOST_TEST((ids == ids_of(*b)));
	BOOST_TEST(ids.size() > 1u);
	BOOST_TEST(std::ranges::none_of(ids, [](uint64_t id) { return id == desc::invalid_node_id; }));
	BOOST_TEST(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
}

BOOST_AUTO_TEST_CASE(Importer_SameNamedSiblingsStayDistinctNodes)
{
	auto a = import("d1/x.gltf", GLTF_SAME_NAMES);
	auto b = import("d2/x.gltf", GLTF_SAME_NAMES);
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(b);

	std::vector<std::string> names;
	collect_names(a->get_root(), names);
	BOOST_TEST(std::ranges::count(names, "Leaf") == 1);
	BOOST_TEST(std::ranges::count_if(names, [](const std::string& n) { return n.starts_with("Dup"); }) == 2);

	auto ids = ids_of(*a);
	BOOST_TEST((ids == ids_of(*b)));
	BOOST_TEST(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
}

BOOST_AUTO_TEST_CASE(Importer_WrittenIdsSurviveSaveAndReload)
{
	auto a = import("d1/x.gltf", GLTF_SAME_NAMES);
	BOOST_REQUIRE(a);
	scn::prefab_desc again;
	again.deserialize(*descs, serialized(*a));
	pump();
	BOOST_TEST((ids_of(*a) == ids_of(again)));
}

#endif

// ---------------------------------------------------------------- node_index

struct index_fixture : node_id_fixture
{
	index_fixture()
	{
		put("inst.prefab.desc", R"({"__type":"prefab_desc","__id":"0000000000000001","children":{
			"hull":{"__id":"0000000000000011","components":{"stat":{"__type":"stat_desc","hp":1}},
				"children":{"gun":{"__id":"0000000000000111"}}}}})");
		world_tag = put("w.desc", R"({"__type":"world_desc","name":"w","systems":[],"__id":"0000000000000002","children":{
			"holder":{"__id":"00000000000000a1","components":{"holder":"memory://t/inst.prefab.desc"}},
			"plain":{"__id":"00000000000000a2","components":{"stat":{"__type":"stat_desc","hp":2}},
				"children":{"inner":{"__id":"00000000000000a3"}}}}})");
		world = load<scn::world_desc>(world_tag);
		BOOST_REQUIRE(world);
		root = reg.create();
		assembler->spawn_from_desc(reg, root, *world, "w");
	}

	entt::entity find(std::initializer_list<uint64_t> path)
	{
		std::vector<uint64_t> v(path);
		return scn::node_index::get(reg).find(v);
	}

	res::tag world_tag;
	std::shared_ptr<scn::world_desc> world;
	entt::registry reg;
	entt::entity root = entt::null;
};

BOOST_FIXTURE_TEST_CASE(Index_ResolvesWorldNodes, index_fixture)
{
	auto plain = find({ 0xa2 });
	BOOST_REQUIRE((plain != entt::null));
	BOOST_TEST(reg.get<scn::name_component>(plain).name == "plain");

	auto inner = find({ 0xa3 });
	BOOST_TEST((inner == entt::null)); // a nested world node is addressed from its parent: [plain, inner]
	inner = find({ 0xa2, 0xa3 });
	BOOST_REQUIRE((inner != entt::null));
	BOOST_TEST(reg.get<scn::name_component>(inner).name == "inner");

	BOOST_TEST((find({}) == root));
	BOOST_TEST((find({ 0xdead }) == entt::null));
}

BOOST_FIXTURE_TEST_CASE(Index_ResolvesPrefabInstanceChildren, index_fixture)
{
	auto holder = find({ 0xa1 });
	BOOST_REQUIRE((holder != entt::null));
	BOOST_TEST(reg.get<scn::name_component>(holder).name == "holder");

	auto hull = find({ 0xa1, 0x11 });
	BOOST_REQUIRE((hull != entt::null));
	BOOST_TEST(reg.get<scn::name_component>(hull).name == "hull");
	BOOST_TEST((reg.get<scn::parent_component>(hull).parent == holder));

	auto gun = find({ 0xa1, 0x11, 0x111 });
	BOOST_REQUIRE((gun != entt::null));
	BOOST_TEST(reg.get<scn::name_component>(gun).name == "gun");
	BOOST_TEST((reg.get<scn::node_id_component>(gun).path == std::vector<uint64_t>{ 0xa1, 0x11, 0x111 }));
}

BOOST_FIXTURE_TEST_CASE(Index_DropsEntitiesWhenTheyAreDestroyed, index_fixture)
{
	auto& index = scn::node_index::get(reg);
	const auto before = index.size();
	auto gun = find({ 0xa1, 0x11, 0x111 });
	BOOST_REQUIRE((gun != entt::null));

	reg.destroy(gun);
	BOOST_TEST((find({ 0xa1, 0x11, 0x111 }) == entt::null));
	BOOST_TEST(index.size() == before - 1);
	BOOST_TEST((find({ 0xa1, 0x11 }) != entt::null));

	reg.clear<scn::node_id_component>();
	BOOST_TEST(index.size() == 0u);
	BOOST_TEST((find({ 0xa2 }) == entt::null));
}

BOOST_FIXTURE_TEST_CASE(Index_FollowsComponentReplacement, index_fixture)
{
	auto plain = find({ 0xa2 });
	BOOST_REQUIRE((plain != entt::null));
	reg.emplace_or_replace<scn::node_id_component>(plain, scn::node_id_component{ { 0x77 } });
	BOOST_TEST((find({ 0xa2 }) == entt::null));
	BOOST_TEST((find({ 0x77 }) == plain));
}

BOOST_AUTO_TEST_SUITE_END()
