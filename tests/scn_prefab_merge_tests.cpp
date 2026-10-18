#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "scn_glm_json_convert.h"
#include <snk/desc/desc_node_id.h>
#include "level/scn_node_index.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

// Behavior of prefab assembly: parse/serialize, hierarchy spawning, override merge for explicit
// nested instances, and hot reload. Descs live in a RAM resolver; no GPU, window or disk.

// Non-namespaced entry point that assembles a prefab_node as an *override* node (nested instance).
void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node);

namespace json = boost::json;

namespace {

constexpr const char* BASE_PREFAB = R"({
	"__type": "prefab_desc", "__version": 2,
	"transform": { "position": [1, 2, 3] },
	"components": { "stat": { "__type": "stat_desc", "hp": 10, "label": "base", "limits": { "min": 1, "max": 10 } } },
	"children": [
		{ "__id": "00000000000000a1", "name": "arm",
		  "transform": { "position": [5, 0, 0], "rotation": [0, 90, 0], "scale": [2, 2, 2] },
		  "components": { "stat": { "__type": "stat_desc", "hp": 3 } } },
		{ "__id": "00000000000000a2", "name": "leg", "components": { "stat": { "__type": "stat_desc", "hp": 4 } } },
		{ "__id": "00000000000000a3", "name": "Light", "components": { "Light": { "__type": "directional_light_desc" } } }
	]
})";

// An explicit nested instance of base.prefab.desc with local overrides. Its records address the base's nodes
// by id (a1 is the base's arm); add_node brings a node of its own.
constexpr const char* INSTANCE_PREFAB = R"({
	"__type": "prefab_desc", "__version": 3,
	"components": {
		"prefab_desc": { "__type": "prefab_desc", "__parent": "memory://t/base.prefab.desc" }
	},
	"overrides": [
		{ "target": [], "component": "stat", "path": "/hp", "value": 99 },
		{ "target": ["00000000000000a1"], "component": "transform", "path": "/position", "value": [7, 0, 0] },
		{ "target": ["00000000000000a1"], "component": "stat", "path": "/hp", "value": 30 },
		{ "target": [], "op": "add_node", "value": { "__id": "00000000000000e1", "name": "extra",
			"components": { "stat": { "__type": "stat_desc", "hp": 1 } } } }
	]
})";

// An instance of the base with the given records (the text between the brackets of "overrides").
std::string instance_with(std::string_view records)
{
	return std::string(R"({"__type":"prefab_desc","__version":3,
		"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"}},
		"overrides":[)") + std::string(records) + "]}";
}

// A record on the base's arm (a1); `body` is the rest of the record after the target.
std::string on_arm(std::string_view body)
{
	return std::string(R"({"target":["00000000000000a1"],)") + std::string(body) + "}";
}

std::string on_leg(std::string_view body)
{
	return std::string(R"({"target":["00000000000000a2"],)") + std::string(body) + "}";
}

glm::vec3 translation_of(entt::registry& reg, entt::entity e)
{
	return glm::vec3(reg.get<scn::local_transform>(e).local[3]);
}

float scale_x_of(entt::registry& reg, entt::entity e)
{
	return glm::length(glm::vec3(reg.get<scn::local_transform>(e).local[0]));
}

entt::entity child_named(entt::registry& reg, entt::entity parent, std::string_view name)
{
	if (auto* children = reg.try_get<scn::children_component>(parent)) {
		for (auto c : children->children) {
			if (auto* n = reg.try_get<scn::name_component>(c); n && n->name == name) return c;
		}
	}
	return entt::null;
}

std::vector<std::string> child_names(entt::registry& reg, entt::entity parent)
{
	std::vector<std::string> out;
	if (auto* children = reg.try_get<scn::children_component>(parent)) {
		for (auto c : children->children) out.push_back(reg.get<scn::name_component>(c).name);
	}
	return out;
}

struct prefab_fixture : test_util::scene_env
{
	// Parses `text` into a standalone prefab_desc (no resource involved).
	scn::prefab_desc parse(const char* text)
	{
		scn::prefab_desc d;
		d.deserialize(*descs, parse_migrated(text));
		return d;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ScnPrefabMergeTests, prefab_fixture)

// ---------------------------------------------------------------- parse / serialize

BOOST_AUTO_TEST_CASE(Parse_ReadsRootTransformComponentsAndChildrenInOrder)
{
	auto d = parse(BASE_PREFAB);
	const auto& root = d.get_root();
	BOOST_TEST((root.position == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST((root.scale == glm::vec3{ 1 }));
	BOOST_REQUIRE(root.components.contains("stat"));
	BOOST_TEST(root.components.at("stat").type_name == "stat_desc");
	BOOST_TEST(root.components.at("stat").overrides.at("hp").as_int64() == 10);

	BOOST_REQUIRE(root.children.size() == 3u);
	BOOST_TEST(root.children[0].name == "arm");
	BOOST_TEST(root.children[1].name == "leg");
	BOOST_TEST(root.children[2].name == "Light");
	BOOST_TEST((root.children[0].rotation == glm::vec3{ 0, 90, 0 }));
	BOOST_TEST((root.children[0].scale == glm::vec3{ 2 }));
}

BOOST_AUTO_TEST_CASE(Parse_V1TypedChildBecomesANodeWithOneComponentNamedAfterIt)
{
	auto d = parse(R"({"children":{"Light":{"__type":"directional_light_desc"}}})");
	const auto& light = d.get_root().children[0];
	BOOST_REQUIRE(light.components.size() == 1u);
	BOOST_REQUIRE(light.components.contains("Light"));
	BOOST_TEST(light.components.at("Light").type_name == "directional_light_desc");
}

BOOST_AUTO_TEST_CASE(Parse_StringComponentBecomesParentReference)
{
	put("mat.desc", R"({"__type":"stat_desc","hp":5})");
	auto d = parse(R"({"components":{"comp":"memory://t/mat.desc"}})");
	const auto& c = d.get_root().components.at("comp");
	BOOST_TEST(c.type_name.empty());
	BOOST_REQUIRE(c.parent_desc.is_valid());
	BOOST_TEST(c.parent_desc.get_tag() == tag_of("mat.desc"));
	BOOST_TEST(c.overrides.empty());
}

BOOST_AUTO_TEST_CASE(Parse_ParentKeepsOverridesButStripsReservedKeys)
{
	put("p.desc", R"({"__type":"stat_desc","hp":5})");
	auto d = parse(R"({"components":{"c":{"__type":"stat_desc","__parent":"memory://t/p.desc","hp":8}}})");
	const auto& c = d.get_root().components.at("c");
	BOOST_TEST(c.type_name == "stat_desc");
	BOOST_TEST(c.parent_desc.is_valid());
	BOOST_TEST(!c.overrides.contains("__type"));
	BOOST_TEST(!c.overrides.contains("__parent"));
	BOOST_TEST(c.overrides.at("hp").as_int64() == 8);
}

BOOST_AUTO_TEST_CASE(Serialize_OmitsDefaultsAndWritesEveryNodeAsAnObjectInAnArray)
{
	auto d = parse(BASE_PREFAB);
	json::object out;
	d.serialize(out);

	BOOST_TEST((json::value_to<glm::vec3>(out.at("transform").at("position")) == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST(!out.at("transform").as_object().contains("scale")); // default scale not written
	BOOST_TEST(out.at("components").at("stat").at("hp").to_number<int>() == 10);

	const auto& children = out.at("children").as_array();
	std::vector<std::string> names;
	for (auto& child : children) names.emplace_back(child.at("name").as_string());
	BOOST_TEST((names == std::vector<std::string>{ "arm", "leg", "Light" }));

	// the typed light stays a component of its node: there is no one-line form
	const auto& light = children[2].as_object();
	BOOST_TEST(light.at("__id").as_string() == "00000000000000a3");
	BOOST_TEST(light.at("components").at("Light").at("__type").as_string() == "directional_light_desc");
	// default transform is not written
	BOOST_TEST(children[1].as_object().contains("components"));
	BOOST_TEST(!children[1].as_object().contains("transform"));
}

BOOST_AUTO_TEST_CASE(Serialize_ThenParse_IsStable)
{
	auto d = parse(BASE_PREFAB);
	json::object first;
	d.serialize(first);

	scn::prefab_desc again;
	again.deserialize(*descs, first);
	json::object second;
	again.serialize(second);

	BOOST_TEST((json::value(first) == json::value(second)));
}

BOOST_AUTO_TEST_CASE(Serialize_ParentOnlyComponentIsWrittenAsTagString)
{
	put("arm.desc", R"({"__type":"stat_desc","hp":5})");
	auto d = parse(R"({"children":{"arm":"memory://t/arm.desc"}})");
	const auto id = d.get_root().children[0].node_id;
	json::object out;
	d.serialize(out);

	const auto& arm = out.at("children").as_array().at(0).as_object();
	BOOST_TEST(arm.at("__id").as_string() == desc::format_node_id(id));
	BOOST_TEST(arm.at("name").as_string() == "arm");
	BOOST_TEST(arm.at("components").at("arm").is_string());
	BOOST_TEST(arm.at("components").at("arm").as_string() == "memory://t/arm.desc");

	scn::prefab_desc again;
	again.deserialize(*descs, out);
	BOOST_TEST(again.get_root().children[0].node_id == id);
	BOOST_TEST(again.get_root().children[0].components.at("arm").parent_desc.get_tag() == tag_of("arm.desc"));
}

BOOST_AUTO_TEST_CASE(PrefabNode_LocalTransformComposesTranslateRotateScale)
{
	scn::prefab_desc::prefab_node n;
	n.position = { 1, 2, 3 };
	n.scale = { 2, 2, 2 };
	auto m = n.get_local_transform();
	BOOST_TEST((glm::vec3(m[3]) == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST(glm::length(glm::vec3(m[0])) == 2.0f, boost::test_tools::tolerance(1e-5f));

	n.rotation = { 0, 90, 0 }; // yaw 90 degrees: local X axis ends up along -Z
	auto r = n.get_local_transform();
	BOOST_TEST(r[0][2] == -2.0f, boost::test_tools::tolerance(1e-4f));
}

// ---------------------------------------------------------------- assemble_prefab

BOOST_AUTO_TEST_CASE(AssemblePrefab_SpawnsRootComponentsChildrenAndHierarchy)
{
	auto tag = put("base.prefab.desc", BASE_PREFAB);
	auto prefab = load<scn::prefab_desc>(tag);
	BOOST_REQUIRE(prefab);

	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "my_root", tag);

	// root
	BOOST_TEST((translation_of(reg, root) == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST(reg.get<scn::name_component>(root).name == "my_root");
	BOOST_TEST(reg.all_of<scn::world_transform>(root));
	BOOST_TEST(reg.all_of<scn::depth_level>(root));
	BOOST_TEST(reg.get<scn::prefab_instance>(root).source_prefab == tag);
	const auto& stat = reg.get<test_util::stat_component>(root);
	BOOST_TEST(stat.hp == 10);
	BOOST_TEST(stat.label == "base");
	BOOST_TEST(stat.limit_min == 1);
	BOOST_TEST(stat.limit_max == 10);

	// children, in declaration order, parented to the root
	BOOST_TEST((child_names(reg, root) == std::vector<std::string>{ "arm", "leg", "Light" }));
	auto arm = child_named(reg, root, "arm");
	auto leg = child_named(reg, root, "leg");
	auto light = child_named(reg, root, "Light");
	for (auto c : { arm, leg, light }) {
		BOOST_REQUIRE(reg.valid(c));
		BOOST_TEST((reg.get<scn::parent_component>(c).parent == root));
		BOOST_TEST(reg.all_of<scn::local_transform>(c));
		BOOST_TEST(reg.all_of<scn::world_transform>(c));
	}
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 5, 0, 0 }));
	BOOST_TEST(scale_x_of(reg, arm) == 2.0f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(reg.get<test_util::stat_component>(arm).hp == 3);
	BOOST_TEST(reg.get<test_util::stat_component>(leg).hp == 4);
	BOOST_TEST(reg.all_of<scn::directional_light>(light)); // shorthand child spawned its typed component
	BOOST_TEST(!reg.all_of<test_util::stat_component>(light));
}

BOOST_AUTO_TEST_CASE(AssemblePrefab_EmitsTransformAndHierarchyEvents)
{
	auto tag = put("base.prefab.desc", BASE_PREFAB);
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);

	auto& transforms = reg.ctx().get<ecs::event<scn::transform_updated>>();
	auto& hierarchy = reg.ctx().get<ecs::event<scn::hierarchy_updated>>();
	// root + three children
	BOOST_TEST(transforms.size() == 4u);
	BOOST_TEST(hierarchy.size() == 4u);
	BOOST_TEST((transforms.list().front() == root));
}

BOOST_AUTO_TEST_CASE(AssemblePrefab_WithoutSourceTagDoesNotMarkInstance)
{
	auto prefab = parse(BASE_PREFAB);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, prefab, "", res::tag{});
	BOOST_TEST(!reg.all_of<scn::prefab_instance>(root));
	BOOST_TEST(!reg.all_of<scn::name_component>(root)); // empty name is not recorded
}

BOOST_AUTO_TEST_CASE(AssemblePrefab_ParentDescComponentIsTrackedWithItsLocalOverrides)
{
	put("stat_a.desc", R"({"__type":"stat_desc","hp":1,"label":"A"})");
	auto prefab = parse(R"({"components":{"stat":{"__type":"stat_desc","__parent":"memory://t/stat_a.desc","hp":50}}})");
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, prefab, "r", res::tag{});

	const auto& s = reg.get<test_util::stat_component>(root);
	BOOST_TEST(s.hp == 50);        // local override
	BOOST_TEST(s.label == "A");    // inherited from the parent desc

	const auto& tracked = reg.get<scn::component_tracker>(root).tracked_components.at("stat");
	BOOST_TEST(tracked.source_desc == tag_of("stat_a.desc"));
	BOOST_TEST(tracked.local_overrides.at("hp").as_int64() == 50);
}

// ---------------------------------------------------------------- nested instance + override merge

BOOST_AUTO_TEST_CASE(NestedInstance_MergesComponentsChildrenAndRecordsOverrides)
{
	auto base_tag = put("base.prefab.desc", BASE_PREFAB);
	auto inst_tag = put("inst.prefab.desc", INSTANCE_PREFAB);
	auto inst = load<scn::prefab_desc>(inst_tag);
	BOOST_REQUIRE(inst);

	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst->get_root());

	// instance bookkeeping
	const auto& pi = reg.get<scn::prefab_instance>(e);
	BOOST_TEST(pi.source_prefab == base_tag);
	BOOST_TEST(pi.local_overrides.overrides.size() == 4u);

	// root component: override value wins, untouched base values survive (top-level keys merge)
	const auto& stat = reg.get<test_util::stat_component>(e);
	BOOST_TEST(stat.hp == 99);
	BOOST_TEST(stat.label == "base");
	BOOST_TEST(stat.limit_max == 10);

	// base children first (in order), override-only children after
	BOOST_TEST((child_names(reg, e) == std::vector<std::string>{ "arm", "leg", "Light", "extra" }));

	auto arm = child_named(reg, e, "arm");
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 7, 0, 0 })); // overridden
	BOOST_TEST(scale_x_of(reg, arm) == 2.0f, boost::test_tools::tolerance(1e-5f)); // inherited from base
	BOOST_TEST(reg.get<test_util::stat_component>(arm).hp == 30);

	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, e, "leg")).hp == 4); // base only
	auto extra = child_named(reg, e, "extra");
	BOOST_REQUIRE(reg.valid(extra));
	BOOST_TEST(reg.get<test_util::stat_component>(extra).hp == 1);        // override only
	BOOST_TEST((translation_of(reg, extra) == glm::vec3{ 0 }));
}

BOOST_AUTO_TEST_CASE(NestedInstance_ChildRotationInheritedWhenOnlyPositionOverridden)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(INSTANCE_PREFAB);
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());

	auto arm = child_named(reg, e, "arm");
	scn::prefab_desc::prefab_node expected;
	expected.position = { 7, 0, 0 };
	expected.rotation = { 0, 90, 0 };
	expected.scale = { 2, 2, 2 };
	const auto m = reg.get<scn::local_transform>(arm).local;
	const auto want = expected.get_local_transform();
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			BOOST_TEST(m[c][r] == want[c][r], boost::test_tools::tolerance(1e-4f));
}

// A record is an override even when it equals the default: the base's arm sits at 5 / yaw 90 / scale 2.
BOOST_AUTO_TEST_CASE(NestedInstance_OverridingPositionBackToZeroIsHonored)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_arm(R"("component":"transform","path":"/position","value":[0,0,0])")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST((translation_of(reg, child_named(reg, e, "arm")) == glm::vec3{ 0 }));
}

BOOST_AUTO_TEST_CASE(NestedInstance_OverridingScaleBackToOneIsHonored)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_arm(R"("component":"transform","path":"/scale","value":[1,1,1])")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST(scale_x_of(reg, child_named(reg, e, "arm")) == 1.0f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(NestedInstance_OverridingRotationBackToZeroIsHonored)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_arm(R"("component":"transform","path":"/rotation","value":[0,0,0])")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	// unrotated: local X axis stays along +X
	BOOST_TEST(reg.get<scn::local_transform>(child_named(reg, e, "arm")).local[0][2] == 0.0f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(NestedInstance_TransformRecordTouchesOnlyItsOwnField)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_arm(R"("component":"transform","path":"/position/1","value":4)")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	auto arm = child_named(reg, e, "arm");
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 5, 4, 0 })); // y replaced, x and z inherited
	BOOST_TEST(scale_x_of(reg, arm) == 2.0f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(NestedInstance_OverrideMergeIsDeepForNestedObjects)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(R"({"target":[],"component":"stat","path":"/limits/max","value":20})").c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	const auto& s = reg.get<test_util::stat_component>(e);
	BOOST_TEST(s.limit_max == 20);
	BOOST_TEST(s.limit_min == 1); // the sibling field of the nested object survives
	BOOST_TEST(s.hp == 10);
	BOOST_TEST(s.label == "base");
}

BOOST_AUTO_TEST_CASE(NestedInstance_RecordAtAMergePointReplacesNothingOutsideItsPointer)
{
	put("base.prefab.desc", BASE_PREFAB);
	// "" merges an object into the component; the keys it does not name keep their base value
	auto inst = parse(instance_with(R"({"target":[],"component":"stat","path":"","value":{"limits":{"min":3}}})").c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	const auto& s = reg.get<test_util::stat_component>(e);
	BOOST_TEST(s.limit_min == 3);
	BOOST_TEST(s.limit_max == 10);
	BOOST_TEST(s.hp == 10);
}

BOOST_AUTO_TEST_CASE(ParentDescOverrides_MergeDeepWithTheSerializedParent)
{
	put("stat_a.desc", R"({"__type":"stat_desc","hp":5,"label":"A","limits":{"min":1,"max":10}})");
	auto prefab = parse(R"({"components":{"stat":{"__type":"stat_desc","__parent":"memory://t/stat_a.desc","limits":{"max":20}}}})");
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, prefab, "r", res::tag{});
	const auto& s = reg.get<test_util::stat_component>(root);
	BOOST_TEST(s.limit_max == 20);
	BOOST_TEST(s.limit_min == 1);
	BOOST_TEST(s.label == "A");
}

BOOST_AUTO_TEST_CASE(NestedInstance_RecordOnAParentDescComponentEditsTheMergedData)
{
	put("stat_a.desc", R"({"__type":"stat_desc","hp":5,"label":"A","limits":{"min":1,"max":10}})");
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000a1","name":"arm","components":{"stat":"memory://t/stat_a.desc"}}]})");
	auto inst = parse(instance_with(on_arm(R"("component":"stat","path":"/limits/max","value":30)")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	const auto& s = reg.get<test_util::stat_component>(child_named(reg, e, "arm"));
	BOOST_TEST(s.limit_max == 30);
	BOOST_TEST(s.limit_min == 1);
	BOOST_TEST(s.hp == 5);
	BOOST_TEST(s.label == "A");
}

BOOST_AUTO_TEST_CASE(NestedInstance_RemoveComponentDropsItFromTheNode)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_arm(R"("op":"remove","component":"stat")")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST(!reg.all_of<test_util::stat_component>(child_named(reg, e, "arm")));
	BOOST_TEST(reg.all_of<test_util::stat_component>(child_named(reg, e, "leg"))); // others untouched
}

BOOST_AUTO_TEST_CASE(NestedInstance_RemoveNodeDropsTheWholeSubtree)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_leg(R"("op":"remove")")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST((child_names(reg, e) == std::vector<std::string>{ "arm", "Light" }));
}

BOOST_AUTO_TEST_CASE(NestedInstance_AddNodeAppendsUnderTheTargetNode)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(on_arm(R"("op":"add_node","value":{"__id":"00000000000000f2","name":"hand",
		"transform":{"position":[0,1,0]},"components":{"stat":{"__type":"stat_desc","hp":8}}})")).c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	auto arm = child_named(reg, e, "arm");
	auto hand = child_named(reg, arm, "hand");
	BOOST_REQUIRE(reg.valid(hand));
	BOOST_TEST(reg.get<test_util::stat_component>(hand).hp == 8);
	BOOST_TEST((translation_of(reg, hand) == glm::vec3{ 0, 1, 0 }));
}

BOOST_AUTO_TEST_CASE(NestedInstance_AddNodeWithAnExistingIdMergesIntoThatNode)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(R"({"target":[],"op":"add_node","value":{"__id":"00000000000000a1","name":"arm",
		"transform":{"position":[9,0,0]}}})").c_str());
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST((child_names(reg, e) == std::vector<std::string>{ "arm", "leg", "Light" }));
	auto arm = child_named(reg, e, "arm");
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 9, 0, 0 }));
	BOOST_TEST(scale_x_of(reg, arm) == 2.0f, boost::test_tools::tolerance(1e-5f)); // what the JSON does not spell out stays
}

BOOST_AUTO_TEST_CASE(NestedInstance_RecordForAMissingNodeOrComponentIsSkipped)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(
		R"({"target":["00000000000000ff"],"component":"stat","path":"/hp","value":1},)"
		R"({"target":[],"component":"nope","path":"/x","value":1},)"
		R"({"target":[],"op":"remove","component":"nope"},)"
		+ on_arm(R"("component":"stat","path":"/hp","value":12)")).c_str());
	entt::registry reg;
	auto e = reg.create();
	BOOST_CHECK_NO_THROW(assemble_prefab_node(*assembler, reg, e, inst.get_root()));
	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, e, "arm")).hp == 12); // the valid record still applies
}

BOOST_AUTO_TEST_CASE(NestedInstance_RecordsReachThroughAnInstanceInsideTheBase)
{
	put("base.prefab.desc", BASE_PREFAB);
	put("mid.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000b1","name":"holder","components":{"base":"memory://t/base.prefab.desc"}}]})");
	// [holder, arm] is a path through the instance of base that holder is
	auto outer = parse(R"({"__type":"prefab_desc","__version":3,
		"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/mid.prefab.desc"}},
		"overrides":[{"target":["00000000000000b1","00000000000000a1"],"component":"transform","path":"/position","value":[3,3,3]}]})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, outer.get_root());
	auto holder = child_named(reg, e, "holder");
	BOOST_REQUIRE(reg.valid(holder));
	BOOST_TEST((translation_of(reg, child_named(reg, holder, "arm")) == glm::vec3{ 3, 3, 3 }));
	BOOST_TEST((translation_of(reg, child_named(reg, holder, "leg")) == glm::vec3{ 0 }));
}

BOOST_AUTO_TEST_CASE(Instance_NodeReferencingAPrefabByStringAppliesItsOwnRecords)
{
	put("base.prefab.desc", BASE_PREFAB);
	// the level-style instance: a node with a plain reference to the prefab and records of its own
	auto level = parse(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","transform":{"position":[0,0,2]},
		 "components":{"base":"memory://t/base.prefab.desc"},
		 "overrides":[
			{"target":["00000000000000a2"],"component":"stat","path":"/hp","value":40},
			{"target":["00000000000000a1"],"component":"transform","path":"/scale","value":[1,1,1]}]},
		{"__id":"00000000000000c2","name":"plain","components":{"base":"memory://t/base.prefab.desc"}}]})");
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, level, "level", res::tag{});

	auto inst = child_named(reg, root, "inst");
	auto plain = child_named(reg, root, "plain");
	BOOST_REQUIRE(reg.valid(inst) && reg.valid(plain));
	BOOST_TEST((translation_of(reg, inst) == glm::vec3{ 0, 0, 2 })); // the node keeps its own transform
	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, inst, "leg")).hp == 40);
	BOOST_TEST(scale_x_of(reg, child_named(reg, inst, "arm")) == 1.0f, boost::test_tools::tolerance(1e-5f));
	// the other instance of the same prefab is not affected
	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, plain, "leg")).hp == 4);
	BOOST_TEST(scale_x_of(reg, child_named(reg, plain, "arm")) == 2.0f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(HotReload_InstanceKeepsItsRecordsWhenTheBaseChanges)
{
	auto base_tag = put("base.prefab.desc", BASE_PREFAB);
	auto level = parse(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","components":{"base":"memory://t/base.prefab.desc"},
		 "overrides":[{"target":["00000000000000a2"],"component":"stat","path":"/hp","value":40}]}]})");
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, level, "level", res::tag{});
	auto inst = child_named(reg, root, "inst");
	BOOST_REQUIRE(reg.valid(inst));
	auto leg = child_named(reg, inst, "leg");
	BOOST_REQUIRE(reg.valid(leg));

	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000a2","name":"leg","components":{"stat":{"__type":"stat_desc","hp":44,"label":"v2"}}}]})");
	rs->signal_changed(base_tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(base_tag));

	BOOST_TEST((child_named(reg, inst, "leg") == leg));
	BOOST_TEST(reg.get<test_util::stat_component>(leg).hp == 40);          // the record still wins over the new base value
	BOOST_TEST(reg.get<test_util::stat_component>(leg).label == "v2");     // what it does not touch follows the base
	BOOST_TEST(!reg.valid(child_named(reg, inst, "arm")));                 // dropped from the base
}

// ---------------------------------------------------------------- records on disk

BOOST_AUTO_TEST_CASE(Records_AreWrittenCanonicalAndSurviveAReload)
{
	// written in a scrambled order; later records at a pointer replace the earlier ones below it
	auto d = parse(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","components":{"base":"memory://t/base.prefab.desc"},"overrides":[
			{"target":["00000000000000a1"],"component":"stat","path":"/limits/max","value":5},
			{"target":["00000000000000a1"],"component":"transform","path":"/scale","value":[1,1,1]},
			{"target":[],"component":"stat","path":"/hp","value":1},
			{"target":["00000000000000a1"],"component":"stat","path":"/limits","value":{"min":0,"max":9}},
			{"target":[],"component":"stat","path":"/hp","value":2},
			{"target":["00000000000000a2"],"component":"stat","path":"/hp","value":3},
			{"target":["00000000000000a2"],"op":"remove","component":"stat"},
			{"target":["00000000000000a3"],"component":"transform","path":"/position","value":[1,1,1]},
			{"target":["00000000000000a3"],"op":"remove"}]}]})");
	json::object out;
	d.serialize(out);
	const auto& records = out.at("children").as_array()[0].at("overrides").as_array();

	std::vector<std::string> summary;
	for (const auto& r : records) {
		const auto& o = r.as_object();
		std::string line = std::to_string(o.at("target").as_array().size());
		line += std::string(" ") + std::string(o.contains("op") ? std::string(o.at("op").as_string()) : "set");
		line += std::string(" ") + std::string(o.contains("component") ? std::string(o.at("component").as_string()) : "-");
		line += std::string(" ") + std::string(o.contains("path") ? std::string(o.at("path").as_string()) : "-");
		summary.push_back(line);
	}
	BOOST_TEST((summary == std::vector<std::string>{
		"0 set stat /hp",           // the later /hp won, the earlier one is gone
		"1 set stat /limits",       // absorbed /limits/max
		"1 set transform /scale",
		"1 set stat /hp",           // a remove never absorbs anything: it stays after the run it follows
		"1 remove stat -",
		"1 set transform /position",
		"1 remove - -",
	}));
	BOOST_TEST(records[0].at("value").to_number<int>() == 2);
	BOOST_TEST(records[0].at("target").as_array().empty());

	// canonical output is a fixed point of load and save
	scn::prefab_desc again;
	again.deserialize(*descs, out);
	json::object second;
	again.serialize(second);
	BOOST_TEST((json::value(out) == json::value(second)));
}

// The records of the instance node "inst" after a save, as a list of "op target-size component path".
std::vector<json::value> saved_records(prefab_fixture& f, const char* overrides)
{
	auto d = f.parse((std::string(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","components":{"base":"memory://t/base.prefab.desc"},"overrides":[)") + overrides + "]}]}").c_str());
	json::object out;
	d.serialize(out);
	const auto& records = out.at("children").as_array()[0].at("overrides").as_array();
	return { records.begin(), records.end() };
}

BOOST_AUTO_TEST_CASE(Records_RemoveAfterAddNodeKeepsBothBecauseAddNodeMayMerge)
{
	// add_node of an existing id merges into the base's node; the remove after it must still delete that node
	auto records = saved_records(*this, R"(
		{"target":[],"op":"add_node","value":{"__id":"00000000000000a1","name":"arm","transform":{"position":[1,0,0]}}},
		{"target":["00000000000000a1"],"component":"transform","path":"/position","value":[2,0,0]},
		{"target":["00000000000000a1"],"op":"remove"})");
	BOOST_REQUIRE(records.size() == 3u);
	BOOST_TEST(records[0].at("op").as_string() == "add_node");
	BOOST_TEST(records[2].at("op").as_string() == "remove");
}

BOOST_AUTO_TEST_CASE(Records_TwoAddNodesOfOneIdAreBothKept)
{
	auto records = saved_records(*this, R"(
		{"target":[],"op":"add_node","value":{"__id":"00000000000000a1","name":"arm","transform":{"position":[1,0,0]}}},
		{"target":[],"op":"add_node","value":{"__id":"00000000000000a1","name":"arm","transform":{"scale":[3,3,3]}}})");
	BOOST_TEST(records.size() == 2u);
}

BOOST_AUTO_TEST_CASE(Records_OrderAgainstRemoveAndAddNodeIsNotChangedBySorting)
{
	auto records = saved_records(*this, R"(
		{"target":["00000000000000a1"],"op":"remove"},
		{"target":[],"op":"add_node","value":{"__id":"00000000000000a1","name":"arm"}},
		{"target":["00000000000000b1","00000000000000b2"],"component":"transform","path":"/position","value":[2,0,0]},
		{"target":["00000000000000b1"],"op":"add_node","value":{"__id":"00000000000000b2","name":"x","transform":{"position":[1,0,0]}}})");
	BOOST_REQUIRE(records.size() == 4u);
	BOOST_TEST(records[0].at("op").as_string() == "remove");
	BOOST_TEST(records[1].at("op").as_string() == "add_node");
	BOOST_TEST(records[2].at("component").as_string() == "transform"); // before the merging add_node, as written
	BOOST_TEST(records[3].at("op").as_string() == "add_node");
}

BOOST_AUTO_TEST_CASE(Records_ArrayPointersAreNeitherCollapsedNorReordered)
{
	auto records = saved_records(*this, R"(
		{"target":[],"component":"stat","path":"/list/-","value":1},
		{"target":[],"component":"stat","path":"/list/-","value":2},
		{"target":[],"component":"stat","path":"/list/10","value":3},
		{"target":[],"component":"stat","path":"/list/2","value":4})");
	BOOST_REQUIRE(records.size() == 4u);
	BOOST_TEST(records[0].at("value").to_number<int>() == 1);
	BOOST_TEST(records[1].at("value").to_number<int>() == 2);
	BOOST_TEST(records[2].at("path").as_string() == "/list/10");
	BOOST_TEST(records[3].at("path").as_string() == "/list/2");
}

BOOST_AUTO_TEST_CASE(Records_AParentPointerDoesNotAbsorbAcrossARemove)
{
	auto records = saved_records(*this, R"(
		{"target":[],"component":"stat","path":"/limits/max","value":5},
		{"target":[],"op":"remove","component":"stat"},
		{"target":[],"component":"stat","path":"","value":{"__type":"stat_desc"}},
		{"target":[],"component":"stat","path":"/limits","value":{"min":1}})");
	BOOST_TEST(records.size() == 4u);
}

BOOST_AUTO_TEST_CASE(Records_PreserveTheOrderOfMergeAndPointerRecordsOfOneComponent)
{
	auto d = parse(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","components":{"base":"memory://t/base.prefab.desc"},"overrides":[
			{"target":[],"component":"stat","path":"/hp","value":1},
			{"target":[],"component":"stat","path":"","value":{"hp":2,"label":"m"}}]}]})");
	json::object out;
	d.serialize(out);
	const auto& records = out.at("children").as_array()[0].at("overrides").as_array();
	BOOST_REQUIRE(records.size() == 2u);
	BOOST_TEST(records[0].at("path").as_string() == "/hp"); // sorting by path would put the merge first and change the result
	BOOST_TEST(records[1].at("path").as_string() == "");
}

BOOST_AUTO_TEST_CASE(Records_MalformedOnesAreSkippedOnLoad)
{
	auto d = parse(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"inst","overrides":[
			5,
			{"target":["zz"],"component":"stat","path":"/hp","value":1},
			{"target":[],"component":"stat","path":"/hp"},
			{"target":[],"op":"explode"},
			{"target":[],"op":"add_node","value":3},
			{"target":[],"component":"stat","path":"/hp","value":1}]}]})");
	BOOST_TEST(d.get_root().children[0].overrides.size() == 1u);
}

// ---------------------------------------------------------------- migration of the v2 instance form

BOOST_AUTO_TEST_CASE(Migration_NestedInstanceLayerBecomesRecords)
{
	// v2: the instance carried a node tree matched against the base by id
	auto migrated = parse_migrated(R"({"__type":"prefab_desc","__version":2,
		"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"},
			"stat":{"__type":"stat_desc","hp":99}},
		"children":[
			{"__id":"00000000000000a1","name":"arm","transform":{"position":[7,0,0]},
			 "components":{"stat":{"__type":"stat_desc","hp":30}}},
			{"__id":"00000000000000e1","name":"extra","components":{"stat":{"__type":"stat_desc","hp":1}}}]})");
	BOOST_TEST(migrated.at("__version").as_int64() == 3);
	BOOST_TEST(!migrated.contains("children"));
	BOOST_TEST(migrated.at("components").as_object().size() == 1u); // only the reference stays a component
	const auto& records = migrated.at("overrides").as_array();
	BOOST_REQUIRE(records.size() == 3u);
	BOOST_TEST(records[0].at("component").as_string() == "stat");
	BOOST_TEST(records[0].at("path").as_string() == "");
	BOOST_TEST(records[1].at("op").as_string() == "add_node");
	BOOST_TEST(records[1].at("value").at("__id").as_string() == "00000000000000a1");

	// and it builds the tree the v2 layer produced
	put("base.prefab.desc", BASE_PREFAB);
	scn::prefab_desc inst;
	inst.deserialize(*descs, migrated);
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());

	const auto& stat = reg.get<test_util::stat_component>(e);
	BOOST_TEST(stat.hp == 99);
	BOOST_TEST(stat.label == "base");
	BOOST_TEST(stat.limit_max == 10);
	BOOST_TEST((child_names(reg, e) == std::vector<std::string>{ "arm", "leg", "Light", "extra" }));
	auto arm = child_named(reg, e, "arm");
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 7, 0, 0 }));
	BOOST_TEST(scale_x_of(reg, arm) == 2.0f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(reg.get<test_util::stat_component>(arm).hp == 30);
	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, e, "extra")).hp == 1);
}

BOOST_AUTO_TEST_CASE(Migration_VersionTwoFileWithoutAnInstanceOnlyGetsTheNewVersion)
{
	const char* text = R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000aa","name":"a","transform":{"position":[1,0,0]}}]})";
	auto migrated = parse_migrated(text);
	BOOST_TEST(migrated.at("__version").as_int64() == 3);
	auto expected = json::parse(text).as_object();
	expected["__version"] = 3;
	BOOST_TEST(json::value(migrated) == json::value(expected));
}

BOOST_AUTO_TEST_CASE(Migration_AnOverlayChildThatIsAnInstanceIsMigratedToo)
{
	auto migrated = parse_migrated(R"({"__type":"prefab_desc","__version":2,
		"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"}},
		"children":[{"__id":"00000000000000e1","name":"holder","components":{
			"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"},
			"stat":{"__type":"stat_desc","hp":5}}}]})");
	const auto& records = migrated.at("overrides").as_array();
	BOOST_REQUIRE(records.size() == 1u);
	const auto& holder = records[0].at("value").as_object();
	BOOST_TEST(holder.at("components").as_object().size() == 1u);        // only the reference is left
	BOOST_REQUIRE(holder.contains("overrides"));
	BOOST_TEST(holder.at("overrides").as_array()[0].at("component").as_string() == "stat");
}

BOOST_AUTO_TEST_CASE(Migration_KeepsRecordsAlreadyOnTheNodeAndReplacesWithAStringReference)
{
	put("stat_a.desc", R"({"__type":"stat_desc","hp":5,"label":"A"})");
	put("base.prefab.desc", BASE_PREFAB);
	auto migrated = parse_migrated(R"({"__type":"prefab_desc","__version":2,
		"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"},
			"stat":"memory://t/stat_a.desc"},
		"overrides":[{"target":[],"component":"stat","path":"/hp","value":1}]})");
	const auto& records = migrated.at("overrides").as_array();
	BOOST_REQUIRE(records.size() == 2u);
	BOOST_TEST(records[0].at("path").as_string() == "/hp");
	BOOST_TEST(records[1].at("path").as_string() == "");

	scn::prefab_desc inst;
	inst.deserialize(*descs, migrated);
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	const auto& s = reg.get<test_util::stat_component>(e);
	BOOST_TEST(s.hp == 5);          // the string reference replaced the base's component (v2 did the same)
	BOOST_TEST(s.label == "A");
}

BOOST_AUTO_TEST_CASE(NestedInstance_MalformedTransformRecordsAreSkipped)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(instance_with(
		on_arm(R"("component":"transform","path":"/position","value":[1,2])") + ","
		+ on_arm(R"("component":"transform","path":"/position/1","value":"x")") + ","
		+ on_arm(R"("component":"transform","path":"","value":{"scale":"big"})")).c_str());
	entt::registry reg;
	auto e = reg.create();
	BOOST_CHECK_NO_THROW(assemble_prefab_node(*assembler, reg, e, inst.get_root()));
	auto arm = child_named(reg, e, "arm");
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 5, 0, 0 }));
	BOOST_TEST(scale_x_of(reg, arm) == 2.0f, boost::test_tools::tolerance(1e-5f));
}

// ---------------------------------------------------------------- placement of an instance

constexpr const char* LEVEL_WITH_INSTANCE = R"({"__type":"prefab_desc","__version":3,"children":[
	{"__id":"00000000000000c1","name":"inst","components":{"base":"memory://t/base.prefab.desc"}}]})";

BOOST_AUTO_TEST_CASE(Instance_WithoutATransformIsPlacedByThePrefabRoot)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto level = parse(LEVEL_WITH_INSTANCE);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, level, "level", res::tag{});
	BOOST_TEST((translation_of(reg, child_named(reg, root, "inst")) == glm::vec3{ 1, 2, 3 }));
}

BOOST_AUTO_TEST_CASE(Instance_DraggedToTheOriginStaysThereAfterSaveAndReload)
{
	put("base.prefab.desc", BASE_PREFAB); // its root sits at 1, 2, 3
	auto level = parse(LEVEL_WITH_INSTANCE);
	level.get_root().children[0].has_transform = true; // what the editor does when the node is moved; position stays 0

	json::object out;
	level.serialize(out);
	const auto& saved = out.at("children").as_array()[0].as_object();
	BOOST_REQUIRE(saved.contains("transform"));
	BOOST_TEST(saved.at("transform").as_object().empty());

	scn::prefab_desc again;
	again.deserialize(*descs, out);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, again, "level", res::tag{});
	BOOST_TEST((translation_of(reg, child_named(reg, root, "inst")) == glm::vec3{ 0 }));
}

BOOST_AUTO_TEST_CASE(Instance_RootTransformRecordPlacesItUnlessTheNodeHasItsOwnTransform)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto level = parse(R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"recorded","components":{"base":"memory://t/base.prefab.desc"},
		 "overrides":[{"target":[],"component":"transform","path":"/position","value":[0,0,7]}]},
		{"__id":"00000000000000c2","name":"placed","transform":{"position":[0,5,0]},"components":{"base":"memory://t/base.prefab.desc"},
		 "overrides":[{"target":[],"component":"transform","path":"/position","value":[0,0,7]}]}]})");
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, level, "level", res::tag{});
	BOOST_TEST((translation_of(reg, child_named(reg, root, "recorded")) == glm::vec3{ 0, 0, 7 }));
	BOOST_TEST((translation_of(reg, child_named(reg, root, "placed")) == glm::vec3{ 0, 5, 0 })); // the node's own transform wins
}

BOOST_AUTO_TEST_CASE(HotReload_InstanceFollowsAChangedPrefabRootTransform)
{
	auto base_tag = put("base.prefab.desc", BASE_PREFAB);
	auto level = parse(LEVEL_WITH_INSTANCE);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, level, "level", res::tag{});
	auto inst = child_named(reg, root, "inst");
	BOOST_REQUIRE((translation_of(reg, inst) == glm::vec3{ 1, 2, 3 }));

	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":3,"transform":{"position":[9,9,9]}})");
	rs->signal_changed(base_tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(base_tag));
	BOOST_TEST((translation_of(reg, inst) == glm::vec3{ 9, 9, 9 }));
}

BOOST_AUTO_TEST_CASE(Migration_ARecordedInstanceIsLeftAlone)
{
	const std::string text = instance_with(on_arm(R"("component":"stat","path":"/hp","value":1)"));
	auto migrated = parse_migrated(text);
	BOOST_TEST(json::value(migrated.at("overrides")) == json::parse(text).at("overrides"));
	BOOST_TEST(!migrated.contains("children"));
}

BOOST_AUTO_TEST_CASE(NestedInstance_MissingBasePrefabStillAppliesOverrides)
{
	auto inst = parse(R"({"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/does_not_exist.desc"},
		"stat":{"__type":"stat_desc","hp":7}}})");
	entt::registry reg;
	auto e = reg.create();
	BOOST_CHECK_NO_THROW(assemble_prefab_node(*assembler, reg, e, inst.get_root()));
	BOOST_TEST(reg.get<test_util::stat_component>(e).hp == 7);
}

// ---------------------------------------------------------------- hot reload of prefab instances

BOOST_AUTO_TEST_CASE(HotReload_UpdatesLiveChildrenInPlaceAddsNewAndRemovesDropped)
{
	auto tag = put("base.prefab.desc", BASE_PREFAB);
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);

	auto arm = child_named(reg, root, "arm");
	auto leg = child_named(reg, root, "leg");
	BOOST_REQUIRE(reg.valid(arm) && reg.valid(leg));

	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,
		"transform": { "position": [1, 2, 3] },
		"components": { "stat": { "__type": "stat_desc", "hp": 11, "label": "v2" } },
		"children": [
			{ "__id": "00000000000000a1", "name": "arm", "transform": { "position": [9, 0, 0] },
			  "components": { "stat": { "__type": "stat_desc", "hp": 33 } } },
			{ "__id": "00000000000000a4", "name": "head", "components": { "stat": { "__type": "stat_desc", "hp": 5 } } },
			{ "__id": "00000000000000a3", "name": "Light", "components": { "Light": { "__type": "directional_light_desc" } } }
		]
	})");
	rs->signal_changed(tag);
	auto changed = rs->require_sync<desc::desc_base>(tag);
	BOOST_REQUIRE(changed.get_sync());

	scn::trigger_hot_reload(*assembler, reg, changed);

	// same entity kept for a child that still exists, with new data
	BOOST_TEST((child_named(reg, root, "arm") == arm));
	BOOST_TEST((translation_of(reg, arm) == glm::vec3{ 9, 0, 0 }));
	BOOST_TEST(reg.get<test_util::stat_component>(arm).hp == 33);
	// root component re-applied
	BOOST_TEST(reg.get<test_util::stat_component>(root).hp == 11);
	BOOST_TEST(reg.get<test_util::stat_component>(root).label == "v2");
	// dropped child destroyed and unlinked, new child created
	BOOST_TEST(!reg.valid(leg));
	auto head = child_named(reg, root, "head");
	BOOST_REQUIRE(reg.valid(head));
	BOOST_TEST(reg.get<test_util::stat_component>(head).hp == 5);
	auto names = child_names(reg, root);
	std::ranges::sort(names);
	BOOST_TEST((names == std::vector<std::string>{ "Light", "arm", "head" }));
}

BOOST_AUTO_TEST_CASE(HotReload_RenamedChildIsRecreatedNotMoved_CurrentBehavior)
{
	// Children are matched by NAME only; a rename is seen as "removed + added", so the live entity
	// (and anything attached to it at runtime) is lost.
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","children":{"arm":{"components":{"stat":{"__type":"stat_desc","hp":3}}}}})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);
	auto arm = child_named(reg, root, "arm");
	reg.emplace<int>(arm, 1234); // runtime-only data

	put("base.prefab.desc", R"({"__type":"prefab_desc","children":{"forearm":{"components":{"stat":{"__type":"stat_desc","hp":3}}}}})");
	rs->signal_changed(tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag));

	BOOST_TEST(!reg.valid(arm));
	auto forearm = child_named(reg, root, "forearm");
	BOOST_REQUIRE(reg.valid(forearm));
	BOOST_TEST(!reg.all_of<int>(forearm));
}

BOOST_AUTO_TEST_CASE(HotReload_ChildKeepsItsEntityWhenRenamedButKeepsItsId)
{
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a1","name":"arm","components":{"stat":{"__type":"stat_desc","hp":3}}}]})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);
	auto arm = child_named(reg, root, "arm");
	reg.emplace<int>(arm, 1234); // runtime-only data

	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a1","name":"forearm","components":{"stat":{"__type":"stat_desc","hp":8}}}]})");
	rs->signal_changed(tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag));

	BOOST_TEST((child_named(reg, root, "forearm") == arm));
	BOOST_TEST(!reg.valid(child_named(reg, root, "arm")));
	BOOST_TEST(reg.get<int>(arm) == 1234);
	BOOST_TEST(reg.get<test_util::stat_component>(arm).hp == 8);
	BOOST_TEST(reg.get<scn::node_id_component>(arm).path == std::vector<uint64_t>{ 0xa1 });
}

BOOST_AUTO_TEST_CASE(HotReload_SameNamedSiblingsAreMatchedByIdNotByName)
{
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a1","name":"x","components":{"stat":{"__type":"stat_desc","hp":1}}},
		{"__id":"00000000000000a2","name":"x","components":{"stat":{"__type":"stat_desc","hp":2}}}]})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);
	const auto before = reg.get<scn::children_component>(root).children;
	BOOST_REQUIRE(before.size() == 2u);

	// the second node comes first and only the first one's hp changes
	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a2","name":"x","components":{"stat":{"__type":"stat_desc","hp":2}}},
		{"__id":"00000000000000a1","name":"x","components":{"stat":{"__type":"stat_desc","hp":10}}}]})");
	rs->signal_changed(tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag));

	const auto after = reg.get<scn::children_component>(root).children;
	BOOST_REQUIRE(after.size() == 2u);
	BOOST_TEST((after == before)); // both entities survived, in their original order
	BOOST_TEST(reg.get<test_util::stat_component>(before[0]).hp == 10);
	BOOST_TEST(reg.get<test_util::stat_component>(before[1]).hp == 2);
}

BOOST_AUTO_TEST_CASE(HotReload_SameNameWithAnotherIdIsANewNode)
{
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a1","name":"arm"}]})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);
	auto arm = child_named(reg, root, "arm");

	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000b1","name":"arm"}]})");
	rs->signal_changed(tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag));

	BOOST_TEST(!reg.valid(arm));
	BOOST_REQUIRE(reg.valid(child_named(reg, root, "arm")));
}

BOOST_AUTO_TEST_CASE(NestedInstance_RecordWithAnotherIdIsAnAdditionEvenWithTheSameName)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(R"({"__type":"prefab_desc","__version":2,
		"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"}},
		"children":[{"__id":"00000000000000f1","name":"arm","components":{"stat":{"__type":"stat_desc","hp":77}}}]})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());

	BOOST_TEST((child_names(reg, e) == std::vector<std::string>{ "arm", "leg", "Light", "arm" }));
	const auto& children = reg.get<scn::children_component>(e).children;
	BOOST_TEST(reg.get<test_util::stat_component>(children[0]).hp == 3);   // the base's arm, untouched
	BOOST_TEST(reg.get<test_util::stat_component>(children[3]).hp == 77);  // the addition
}

BOOST_AUTO_TEST_CASE(AssemblePrefab_FillsNodeIndexWithDistinctPathsForSameNamedSiblings)
{
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a1","name":"x"},{"__id":"00000000000000a2","name":"x"}]})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);

	const auto& index = scn::node_index::get(reg);
	const auto first = index.find(std::vector<uint64_t>{ 0xa1 });
	const auto second = index.find(std::vector<uint64_t>{ 0xa2 });
	BOOST_REQUIRE((first != entt::null && second != entt::null));
	BOOST_TEST((first != second));
}

BOOST_AUTO_TEST_CASE(HotReload_ReplacesChildrenEvenWhenNewVersionKeepsOnlyRenamedOnes)
{
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","children":{"a":{"components":{"stat":{"__type":"stat_desc"}}},"b":{"components":{"stat":{"__type":"stat_desc"}}}}})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);

	put("base.prefab.desc", R"({"__type":"prefab_desc","children":{"b":{"components":{"stat":{"__type":"stat_desc"}}},"c":{"components":{"stat":{"__type":"stat_desc"}}}}})");
	rs->signal_changed(tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag));

	auto names = child_names(reg, root);
	std::ranges::sort(names);
	BOOST_TEST((names == std::vector<std::string>{ "b", "c" }));
}

BOOST_AUTO_TEST_CASE(HotReload_RemovingAllChildrenDestroysLiveChildren)
{
	auto tag = put("base.prefab.desc", R"({"__type":"prefab_desc","children":{"arm":{"components":{"stat":{"__type":"stat_desc","hp":3}}}}})");
	auto prefab = load<scn::prefab_desc>(tag);
	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *prefab, "r", tag);
	auto arm = child_named(reg, root, "arm");

	put("base.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	rs->signal_changed(tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag));

	BOOST_TEST(!reg.valid(arm));
}

BOOST_AUTO_TEST_CASE(HotReload_OnlyReloadsInstancesOfTheChangedPrefab)
{
	auto tag_a = put("a.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	auto tag_b = put("b.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":1}}})");
	entt::registry reg;
	auto ea = reg.create();
	auto eb = reg.create();
	scn::assemble_prefab(*assembler, reg, ea, *load<scn::prefab_desc>(tag_a), "a", tag_a);
	scn::assemble_prefab(*assembler, reg, eb, *load<scn::prefab_desc>(tag_b), "b", tag_b);

	put("a.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":2}}})");
	rs->signal_changed(tag_a);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(tag_a));

	BOOST_TEST(reg.get<test_util::stat_component>(ea).hp == 2);
	BOOST_TEST(reg.get<test_util::stat_component>(eb).hp == 1);
}

BOOST_AUTO_TEST_CASE(HotReload_NestedInstanceSeesChangedBasePrefab)
{
	auto base_tag = put("base.prefab.desc", BASE_PREFAB);
	auto inst = load<scn::prefab_desc>(put("inst.prefab.desc", INSTANCE_PREFAB));
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst->get_root());
	BOOST_REQUIRE(reg.get<test_util::stat_component>(child_named(reg, e, "leg")).hp == 4);

	put("base.prefab.desc", R"({"__type":"prefab_desc","__version":2,"children":[
		{"__id":"00000000000000a2","name":"leg","components":{"stat":{"__type":"stat_desc","hp":44}}}]})");
	rs->signal_changed(base_tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(base_tag));

	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, e, "leg")).hp == 44);
}

// Nested assembly resolves its parent through the assembler's injected resource system.
BOOST_AUTO_TEST_CASE(NestedInstance_AssemblesViaInjectedResourceSystem)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = load<scn::prefab_desc>(put("inst.prefab.desc", INSTANCE_PREFAB));
	BOOST_REQUIRE(inst);

	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst->get_root());

	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, e, "leg")).hp == 4); // from the parent
	BOOST_TEST(reg.get<test_util::stat_component>(e).hp == 99);                         // local override
}

// #82: a node that only references an asset (e.g. a .glb) must keep its own transform at spawn,
// otherwise the editor syncs the identity back into the desc and Save loses the transform.
BOOST_AUTO_TEST_CASE(AssemblePrefab_NodeReferencingAssetKeepsItsOwnTransform)
{
	put("asset.prefab.desc", R"({"__type":"prefab_desc","__version":3,"components":{"stat":{"__type":"stat_desc","hp":2}}})");
	auto world_tag = put("w.desc", R"({"__type":"world_desc","__version":3,"name":"w","systems":[],"children":[
		{"__id":"00000000000000b1","name":"scene","transform":{"position":[0,-1,-6],"scale":[0.1,0.1,0.1]},
			"components":{"scene":"memory://t/asset.prefab.desc"}},
		{"__id":"00000000000000b2","name":"plain","components":{"plain":"memory://t/asset.prefab.desc"}}]})");
	auto world = load<scn::world_desc>(world_tag);
	BOOST_REQUIRE(world);

	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *world, "w", world_tag);

	auto scene = child_named(reg, root, "scene");
	BOOST_REQUIRE(scene != entt::null);
	BOOST_TEST((translation_of(reg, scene) == glm::vec3{ 0, -1, -6 }));
	BOOST_TEST(scale_x_of(reg, scene) == 0.1f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(reg.get<test_util::stat_component>(scene).hp == 2); // the referenced content is still assembled
}

// #172: the node's name stays when its reference component has another key.
BOOST_AUTO_TEST_CASE(AssemblePrefab_NodeNameSurvivesADifferentReferenceKey)
{
	put("asset.prefab.desc", R"({"__type":"prefab_desc","__version":3,"components":{"stat":{"__type":"stat_desc","hp":2}}})");
	auto world_tag = put("w.desc", R"({"__type":"world_desc","__version":3,"name":"w","systems":[],"children":[
		{"__id":"00000000000000b1","name":"robot","components":{"model_key":"memory://t/asset.prefab.desc"}}]})");
	auto world = load<scn::world_desc>(world_tag);
	BOOST_REQUIRE(world);

	entt::registry reg;
	auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *world, "w", world_tag);

	BOOST_TEST((child_named(reg, root, "model_key") == entt::null));
	auto robot = child_named(reg, root, "robot");
	BOOST_REQUIRE(robot != entt::null);
	BOOST_TEST(reg.get<test_util::stat_component>(robot).hp == 2);
}

BOOST_AUTO_TEST_SUITE_END()
