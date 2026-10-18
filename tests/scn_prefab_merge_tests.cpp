#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "scn_glm_json_convert.h"
#include "desc_node_id.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

// Behavior of prefab assembly: parse/serialize, hierarchy spawning, override merge for explicit
// nested instances, and hot reload. Descs live in a RAM resolver; no GPU, window or disk.

// Legacy global; the test below nulls it to prove prefab assembly no longer reads it.
extern res::resource_system* p_res_system;
// Non-namespaced entry point that assembles a prefab_node as an *override* node (nested instance).
void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node);

namespace json = boost::json;

namespace {

constexpr const char* BASE_PREFAB = R"({
	"__type": "prefab_desc",
	"transform": { "position": [1, 2, 3] },
	"components": { "stat": { "__type": "stat_desc", "hp": 10, "label": "base", "limits": { "min": 1, "max": 10 } } },
	"children": {
		"arm": { "transform": { "position": [5, 0, 0], "rotation": [0, 90, 0], "scale": [2, 2, 2] },
		         "components": { "stat": { "__type": "stat_desc", "hp": 3 } } },
		"leg": { "components": { "stat": { "__type": "stat_desc", "hp": 4 } } },
		"Light": { "__type": "directional_light_desc" }
	}
})";

// An explicit nested instance of base.prefab.desc with local overrides.
constexpr const char* INSTANCE_PREFAB = R"({
	"__type": "prefab_desc",
	"components": {
		"prefab_desc": { "__type": "prefab_desc", "__parent": "memory://t/base.prefab.desc" },
		"stat": { "__type": "stat_desc", "hp": 99 }
	},
	"children": {
		"arm": { "transform": { "position": [7, 0, 0] }, "components": { "stat": { "__type": "stat_desc", "hp": 30 } } },
		"extra": { "components": { "stat": { "__type": "stat_desc", "hp": 1 } } }
	}
})";

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
		d.deserialize(*descs, json::parse(text).as_object());
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

BOOST_AUTO_TEST_CASE(Parse_TypedChildIsShorthandForSingleComponentNamedAfterNode)
{
	auto d = parse(BASE_PREFAB);
	const auto& light = d.get_root().children[2];
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

BOOST_AUTO_TEST_CASE(Serialize_OmitsDefaultsAndCollapsesShorthandNodes)
{
	auto d = parse(BASE_PREFAB);
	// Loaded nodes carry an id and only id-less nodes collapse (the one-line form cannot hold it).
	d.get_root().children[2].node_id = 0;
	json::object out;
	d.serialize(out);

	BOOST_TEST((json::value_to<glm::vec3>(out.at("transform").at("position")) == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST(!out.at("transform").as_object().contains("scale")); // default scale not written
	BOOST_TEST(out.at("components").at("stat").at("hp").to_number<int>() == 10);

	const auto& children = out.at("children").as_object();
	std::vector<std::string> keys;
	for (auto& kv : children) keys.emplace_back(kv.key());
	BOOST_TEST((keys == std::vector<std::string>{ "arm", "leg", "Light" }));

	// "Light" collapses back to the one-line typed form
	BOOST_TEST(children.at("Light").at("__type").as_string() == "directional_light_desc");
	BOOST_TEST(!children.at("Light").as_object().contains("components"));
	// "leg" has default transform but its component name differs from the node name -> not collapsed
	BOOST_TEST(children.at("leg").as_object().contains("components"));
	BOOST_TEST(!children.at("leg").as_object().contains("transform"));
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

BOOST_AUTO_TEST_CASE(Serialize_ParentOnlyComponentCollapsesToTagString)
{
	put("arm.desc", R"({"__type":"stat_desc","hp":5})");
	auto d = parse(R"({"children":{"arm":"memory://t/arm.desc"}})");
	// Loaded nodes always carry an id and the collapsed form has no room for it: only an id-less node collapses.
	d.get_root().children[0].node_id = 0;
	json::object out;
	d.serialize(out);

	BOOST_TEST(out.at("children").at("arm").is_string());
	BOOST_TEST(out.at("children").at("arm").as_string() == "memory://t/arm.desc");

	scn::prefab_desc again;
	again.deserialize(*descs, out);
	BOOST_TEST(again.get_root().children[0].components.at("arm").parent_desc.get_tag() == tag_of("arm.desc"));
}

BOOST_AUTO_TEST_CASE(Serialize_ParentOnlyComponentOfNodeWithIdKeepsTheIdInsteadOfCollapsing)
{
	put("arm.desc", R"({"__type":"stat_desc","hp":5})");
	auto d = parse(R"({"children":{"arm":"memory://t/arm.desc"}})");
	const auto id = d.get_root().children[0].node_id;
	json::object out;
	d.serialize(out);

	const auto& arm = out.at("children").at("arm").as_object();
	BOOST_TEST(arm.at("__id").as_string() == desc::format_node_id(id));
	BOOST_TEST(arm.at("components").at("arm").as_string() == "memory://t/arm.desc");

	scn::prefab_desc again;
	again.deserialize(*descs, out);
	BOOST_TEST(again.get_root().children[0].node_id == id);
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
	BOOST_TEST(pi.local_overrides.children.size() == 2u);

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

// BUG: scn_prefab_desc.cpp ~236-244 -- transform overrides are applied with a sentinel compare
// (`!= vec3(0)` / `!= vec3(1)`), so an override that explicitly sets position 0 / rotation 0 /
// scale 1 is indistinguishable from "not overridden" and the BASE value leaks through.
BOOST_TEST_DECORATOR(*boost::unit_test::expected_failures(1))
BOOST_AUTO_TEST_CASE(NestedInstance_OverridingPositionBackToZeroIsHonored)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(R"({"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"}},
		"children":{"arm":{"transform":{"position":[0,0,0]}}}})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST((translation_of(reg, child_named(reg, e, "arm")) == glm::vec3{ 0 }));
}

// BUG: same sentinel compare, scale side (explicit scale 1 cannot undo a base scale of 2).
BOOST_TEST_DECORATOR(*boost::unit_test::expected_failures(1))
BOOST_AUTO_TEST_CASE(NestedInstance_OverridingScaleBackToOneIsHonored)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(R"({"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"}},
		"children":{"arm":{"transform":{"scale":[1,1,1]}}}})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	BOOST_TEST(scale_x_of(reg, child_named(reg, e, "arm")) == 1.0f, boost::test_tools::tolerance(1e-5f));
}

// BUG: same sentinel compare, rotation side.
BOOST_TEST_DECORATOR(*boost::unit_test::expected_failures(1))
BOOST_AUTO_TEST_CASE(NestedInstance_OverridingRotationBackToZeroIsHonored)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(R"({"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"}},
		"children":{"arm":{"transform":{"rotation":[0,0,0]}}}})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	// unrotated: local X axis stays along +X
	BOOST_TEST(reg.get<scn::local_transform>(child_named(reg, e, "arm")).local[0][2] == 0.0f, boost::test_tools::tolerance(1e-4f));
}

// BUG: scn_prefab_desc.cpp ~207-208 -- override merge is SHALLOW per top-level key: overriding one
// field of a nested object ("limits.max") replaces the whole object, so "limits.min" from the
// base is lost (falls back to the type default 0 instead of staying 1).
BOOST_TEST_DECORATOR(*boost::unit_test::expected_failures(1))
BOOST_AUTO_TEST_CASE(NestedInstance_OverrideMergeIsDeepForNestedObjects)
{
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(R"({"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"},
		"stat":{"__type":"stat_desc","limits":{"max":20}}}})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	const auto& s = reg.get<test_util::stat_component>(e);
	BOOST_TEST(s.limit_max == 20);
	BOOST_TEST(s.limit_min == 1); // would hold with a deep merge
}

BOOST_AUTO_TEST_CASE(NestedInstance_ShallowMergeReplacesWholeNestedObject_CurrentBehavior)
{
	// Characterizes the CURRENT (shallow) behavior behind the expected failure above.
	put("base.prefab.desc", BASE_PREFAB);
	auto inst = parse(R"({"components":{"prefab_desc":{"__type":"prefab_desc","__parent":"memory://t/base.prefab.desc"},
		"stat":{"__type":"stat_desc","limits":{"max":20}}}})");
	entt::registry reg;
	auto e = reg.create();
	assemble_prefab_node(*assembler, reg, e, inst.get_root());
	const auto& s = reg.get<test_util::stat_component>(e);
	BOOST_TEST(s.limit_max == 20);
	BOOST_TEST(s.hp == 10);    // other top-level keys still merge
	BOOST_TEST(s.label == "base");
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

	put("base.prefab.desc", R"({"__type":"prefab_desc",
		"transform": { "position": [1, 2, 3] },
		"components": { "stat": { "__type": "stat_desc", "hp": 11, "label": "v2" } },
		"children": {
			"arm": { "transform": { "position": [9, 0, 0] }, "components": { "stat": { "__type": "stat_desc", "hp": 33 } } },
			"head": { "components": { "stat": { "__type": "stat_desc", "hp": 5 } } },
			"Light": { "__type": "directional_light_desc" }
		}
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

	put("base.prefab.desc", R"({"__type":"prefab_desc","children":{"leg":{"components":{"stat":{"__type":"stat_desc","hp":44}}}}})");
	rs->signal_changed(base_tag);
	scn::trigger_hot_reload(*assembler, reg, rs->require_sync<desc::desc_base>(base_tag));

	BOOST_TEST(reg.get<test_util::stat_component>(child_named(reg, e, "leg")).hp == 44);
}

// Nested assembly resolves its parent through the assembler's injected resource system, not the
// p_res_system global, so it must work with the global unset.
BOOST_AUTO_TEST_CASE(NestedInstance_AssemblesWithGlobalResourceSystemUnset)
{
	struct restore_global
	{
		res::resource_system* prev;
		~restore_global() { p_res_system = prev; }
	} restore{ p_res_system };
	p_res_system = nullptr;

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
	put("asset.prefab.desc", R"({"__type":"prefab_desc","components":{"stat":{"__type":"stat_desc","hp":2}}})");
	auto world_tag = put("w.desc", R"({"__type":"world_desc","name":"w","systems":[],"children":{
		"scene":{"transform":{"position":[0,-1,-6],"scale":[0.1,0.1,0.1]},"components":{"scene":"memory://t/asset.prefab.desc"}},
		"plain":"memory://t/asset.prefab.desc"}})");
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

BOOST_AUTO_TEST_SUITE_END()
