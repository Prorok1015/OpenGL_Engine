#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "adapters/scn_model_importer_adapter.h"
#include "desc_node_id.h"
#include "level/scn_node_index.h"
#include "edt_scene_editor.h"
#include "edt_shared_state.h"
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

void collect_names(const prefab_node& node, std::vector<std::string>& out)
{
	out.push_back(node.name);
	for (const auto& child : node.children) collect_names(child, out);
}

struct node_id_fixture : test_util::scene_env
{
	node_id_fixture()
	{
		rs->registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(*descs), std::ref(*rs));
	}

	scn::prefab_desc parse(const char* text)
	{
		scn::prefab_desc d;
		d.deserialize(*descs, parse_migrated(text));
		return d;
	}

	json::object serialized(const scn::prefab_desc& d)
	{
		json::object o;
		d.serialize(o);
		return o;
	}

	std::shared_ptr<scn::prefab_desc> import(const char* name, const char* text)
	{
		auto tag = res::tag{ std::string("memory://models/") + name };
		rs->store(tag, test_util::to_bytes(text));
		return rs->require_sync<scn::prefab_desc>(tag).get_sync();
	}
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

	edt::shared_state state;
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

BOOST_FIXTURE_TEST_CASE(Editor_DuplicateGivesTheWholeSubtreeFreshIds, editor_fixture)
{
	editor->duplicate_entity("arm");
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
	editor->duplicate_entities({ "arm", "leg" });
	std::vector<uint64_t> ids;
	collect_ids(world().get_root(), ids);
	BOOST_TEST(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());

	editor->reparent_entity("leg", "arm", -1);
	const auto* leg = edt::scene_editor::find_node_by_name(world().get_root().children, "leg");
	BOOST_REQUIRE(leg);
	BOOST_TEST(leg->node_id == 0xddull);
}

// ---------------------------------------------------------------- importer

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
	BOOST_TEST((ids_of(*a) == ids_of(again)));
}

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
