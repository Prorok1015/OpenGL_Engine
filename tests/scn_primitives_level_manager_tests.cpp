#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "scn_primitives.h"
#include "level/scn_level_manager.h"
#include "adapters/scn_worldwrap_adapter.h"
#include <cmath>
#include <set>
#include <tuple>

// Procedural primitives (pure math), level_manager load/hot-reload policy and the world wrapper
// adapter. All headless.

namespace {

glm::vec3 tri_normal(const std::vector<scn::vertex>& v, const std::vector<unsigned>& idx, size_t t)
{
	const auto& a = v[idx[t * 3 + 0]].position;
	const auto& b = v[idx[t * 3 + 1]].position;
	const auto& c = v[idx[t * 3 + 2]].position;
	return glm::cross(b - a, c - a);
}

}

BOOST_AUTO_TEST_SUITE(ScnPrimitivesTests)

BOOST_AUTO_TEST_CASE(Web_BuildsOneLinePerGridStepInBothDirections)
{
	auto web = scn::generate_web({ 4, 6 });
	// (4/2*2+1) = 5 lines along X, (6/2*2+1) = 7 lines along Z, two vertices each
	BOOST_TEST(web.vertices.size() == 24u);
	BOOST_TEST(web.indices.size() == 24u);
	for (size_t i = 0; i < web.indices.size(); ++i) BOOST_TEST(web.indices[i] == i); // pure line list

	// first line: x = -1, from z=-1 to z=+1 on the ground plane
	BOOST_TEST((web.vertices[0].position == glm::vec3{ -1, 0, -1 }));
	BOOST_TEST((web.vertices[1].position == glm::vec3{ -1, 0, 1 }));
	// middle X line is at x = 0
	BOOST_TEST(web.vertices[4].position.x == 0.0f);
	// last X line at x = +1
	BOOST_TEST(web.vertices[8].position.x == 1.0f);
	// first line of the second family spans x from +1 to -1 at z = -1
	BOOST_TEST((web.vertices[10].position == glm::vec3{ 1, 0, -1 }));
	BOOST_TEST((web.vertices[11].position == glm::vec3{ -1, 0, -1 }));
}

BOOST_AUTO_TEST_CASE(Web_OddSizeRoundsDownToEvenGrid)
{
	auto odd = scn::generate_web({ 5, 5 });
	auto even = scn::generate_web({ 4, 4 });
	BOOST_TEST(odd.vertices.size() == even.vertices.size());
}

BOOST_AUTO_TEST_CASE(Cube_HasEightCornersTwelveOutwardFacingTriangles)
{
	auto cube = scn::generate_cube();
	BOOST_REQUIRE(cube.vertices.size() == 8u);
	BOOST_REQUIRE(cube.indices.size() == 36u);
	for (auto i : cube.indices) BOOST_TEST(i < 8u);
	for (auto& v : cube.vertices) {
		BOOST_TEST(std::abs(v.position.x) == 1.0f);
		BOOST_TEST(std::abs(v.position.y) == 1.0f);
		BOOST_TEST(std::abs(v.position.z) == 1.0f);
	}

	std::set<std::tuple<int, int, int>> face_directions;
	for (size_t t = 0; t < 12; ++t) {
		auto n = tri_normal(cube.vertices, cube.indices, t);
		const auto& a = cube.vertices[cube.indices[t * 3]].position;
		BOOST_TEST(glm::length(n) == 4.0f, boost::test_tools::tolerance(1e-5f)); // half of a 2x2 face
		BOOST_TEST(glm::dot(n, a) > 0.0f);                                          // CCW seen from outside
		auto d = glm::normalize(n);
		face_directions.emplace((int)std::lround(d.x), (int)std::lround(d.y), (int)std::lround(d.z));
	}
	BOOST_TEST(face_directions.size() == 6u); // all six faces are covered
}

BOOST_AUTO_TEST_CASE(Cube_VertexNormalsAreUnitLengthAxisAligned)
{
	auto cube = scn::generate_cube();
	for (auto& v : cube.vertices) {
		BOOST_TEST(glm::length(v.normal) == 1.0f, boost::test_tools::tolerance(1e-5f));
		int non_zero = (v.normal.x != 0) + (v.normal.y != 0) + (v.normal.z != 0);
		BOOST_TEST(non_zero == 1);
	}
}

BOOST_AUTO_TEST_CASE(Sphere_IsAUnitSphereWithOutwardNormalsAndValidIndices)
{
	auto s = scn::generate_sphere();
	BOOST_TEST(s.vertices.size() == 73u * 25u);        // (sectors+1) * (stacks+1)
	BOOST_TEST(s.indices.size() == 72u * 46u * 3u);    // 2 tris per quad, 1 on each pole stack
	BOOST_TEST(s.indices.size() % 3 == 0u);
	for (auto i : s.indices) BOOST_REQUIRE(i < s.vertices.size());

	for (auto& v : s.vertices) {
		BOOST_REQUIRE(glm::length(v.position) == 1.0f || std::abs(glm::length(v.position) - 1.0f) < 1e-5f);
		BOOST_REQUIRE(glm::length(v.normal - v.position) < 1e-5f);
	}
}

BOOST_AUTO_TEST_CASE(Sphere_HasNoDegenerateTrianglesAndConsistentWinding)
{
	auto s = scn::generate_sphere();
	size_t outward = 0, inward = 0;
	for (size_t t = 0; t < s.indices.size() / 3; ++t) {
		auto n = tri_normal(s.vertices, s.indices, t);
		BOOST_REQUIRE(glm::length(n) > 1e-6f);
		const auto& a = s.vertices[s.indices[t * 3]].position;
		(glm::dot(n, a) > 0 ? outward : inward)++;
	}
	// all triangles must agree on the winding direction, otherwise back-face culling eats part of the mesh
	BOOST_TEST((outward == 0u || inward == 0u));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- level_manager + worldwrap_adapter

namespace {

constexpr const char* LEVEL_V1 = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[],"children":{"c1":{"components":{"stat":{"__type":"stat_desc","hp":1}}}}},
	{"__type":"world_desc","name":"w1","systems":[]}]})";

// same world names, different content/systems
constexpr const char* LEVEL_V2_SAME_NAMES = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[],"children":{"c2":{"components":{"stat":{"__type":"stat_desc","hp":2}}}}},
	{"__type":"world_desc","name":"w1","systems":[]}]})";

constexpr const char* LEVEL_V3_DIFFERENT_WORLDS = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"only","systems":[],"children":{"c3":{"components":{"stat":{"__type":"stat_desc","hp":3}}}}}]})";

struct manager_fixture : test_util::scene_env
{
	manager_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{}

	static bool has_named(entt::registry& reg, std::string_view name)
	{
		for (auto [e, n] : reg.view<scn::name_component>().each()) {
			if (n.name == name) return true;
		}
		return false;
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::level_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnLevelManagerTests, manager_fixture)

BOOST_AUTO_TEST_CASE(Load_BuildsTheLevelFromADescTag)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_TEST(mgr.load(tag));
	auto& lvl = mgr.get_level();
	BOOST_TEST(lvl.get_world_count() == 2u);
	BOOST_TEST(lvl.get_tag() == tag);
	BOOST_TEST(has_named(lvl.get_world("w0").state(), "c1"));
}

BOOST_AUTO_TEST_CASE(Load_MissingLevelReturnsFalseAndKeepsCurrentLevel)
{
	BOOST_REQUIRE(mgr.load(put("lv.desc", LEVEL_V1)));
	BOOST_TEST(!mgr.load(tag_of("does_not_exist.desc")));
	BOOST_TEST(mgr.get_level().get_world_count() == 2u);
	BOOST_TEST(has_named(mgr.get_level().get_world("w0").state(), "c1"));
}

BOOST_AUTO_TEST_CASE(Load_SecondLevelReplacesTheFirst)
{
	BOOST_REQUIRE(mgr.load(put("lv.desc", LEVEL_V1)));
	BOOST_REQUIRE(mgr.load(put("lv3.desc", LEVEL_V3_DIFFERENT_WORLDS)));
	auto& lvl = mgr.get_level();
	BOOST_TEST(lvl.get_world_count() == 1u);
	BOOST_TEST(lvl.has_world("only"));
	BOOST_TEST(!lvl.has_world("w0"));
}

BOOST_AUTO_TEST_CASE(HotReload_SameWorldNamesOnlyReloadSystemsNotContent)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load(tag));

	put("lv.desc", LEVEL_V2_SAME_NAMES);
	rs->signal_changed(tag);
	mgr.update(std::chrono::duration<float>(0.001f));

	auto& reg = mgr.get_level().get_world("w0").state();
	BOOST_TEST(has_named(reg, "c1"));    // existing entities are kept...
	BOOST_TEST(!has_named(reg, "c2"));   // ...the new content is NOT spawned (only systems are re-read)
}

BOOST_AUTO_TEST_CASE(HotReload_ChangedWorldListTriggersFullReload)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load(tag));

	put("lv.desc", LEVEL_V3_DIFFERENT_WORLDS);
	rs->signal_changed(tag);
	mgr.update(std::chrono::duration<float>(0.001f));

	auto& lvl = mgr.get_level();
	BOOST_TEST(lvl.get_world_count() == 1u);
	BOOST_TEST(lvl.has_world("only"));
	BOOST_TEST(has_named(lvl.get_world("only").state(), "c3"));
}

// BUG: same as HotReloadManager_UnwatchedNonPrefabDescDoesNotAbort -- after the old level file is
// unwatched, its change falls to the internal reload and trigger_hot_reload's polymorphic cast of a
// level_desc to prefab_desc asserts (abort) in Debug builds.
BOOST_TEST_DECORATOR(*boost::unit_test::disabled())
BOOST_AUTO_TEST_CASE(HotReload_ReloadingAnotherLevelStopsWatchingTheOldOne)
{
	auto tag1 = put("lv.desc", LEVEL_V1);
	auto tag3 = put("lv3.desc", LEVEL_V3_DIFFERENT_WORLDS);
	BOOST_REQUIRE(mgr.load(tag1));
	BOOST_REQUIRE(mgr.load(tag3));

	// the first level's file changes afterwards: must not tear down the active level
	put("lv.desc", LEVEL_V2_SAME_NAMES);
	rs->signal_changed(tag1);
	mgr.update(std::chrono::duration<float>(0.001f));
	BOOST_TEST(mgr.get_level().has_world("only"));
	BOOST_TEST(mgr.get_level().get_world_count() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(ScnWorldWrapAdapterTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(WrapsAnyDescFileAsAWorldDesc)
{
	rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
	auto tag = put("prefab_as_world.desc", R"({"__type":"prefab_desc","children":{"c":{"components":{"stat":{"__type":"stat_desc","hp":4}}}}})");

	auto world = load<scn::world_desc>(tag);
	BOOST_REQUIRE(world);
	BOOST_TEST(world->get_type() == "world_desc");
	BOOST_REQUIRE(world->get_root().children.size() == 1u);
	BOOST_TEST(world->get_root().children[0].name == "c");
}

BOOST_AUTO_TEST_CASE(WorldTypedFileKeepsItsNameSystemsAndInheritsParent)
{
	rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
	put("parent_world.desc", R"({"__type":"world_desc","name":"base","systems":["a","b"],"children":{"p":{"components":{"stat":{"__type":"stat_desc","hp":1}}}}})");
	auto child = load<scn::world_desc>(put("child_world.desc",
		R"({"__type":"world_desc","__parent":"memory://t/parent_world.desc","name":"derived"})"));

	BOOST_REQUIRE(child);
	BOOST_TEST(child->get_name() == "derived");                                  // own field wins
	BOOST_TEST((child->get_systems() == std::vector<std::string>{ "a", "b" })); // inherited
}

// BUG: scn_prefab_desc.cpp prefab_desc::deserialize -- `root = parse_node(...)` replaces the whole
// tree after desc_adapter/worldwrap_adapter already copied the __parent into the instance, so a
// derived prefab/world silently loses every inherited component and child (only the scalar
// fields name/systems of world_desc are inherited).
BOOST_TEST_DECORATOR(*boost::unit_test::expected_failures(1))
BOOST_AUTO_TEST_CASE(WorldTypedFileInheritsParentContent)
{
	rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
	put("parent_world.desc", R"({"__type":"world_desc","name":"base","children":{"p":{"components":{"stat":{"__type":"stat_desc","hp":1}}}}})");
	auto child = load<scn::world_desc>(put("child_world.desc",
		R"({"__type":"world_desc","__parent":"memory://t/parent_world.desc","name":"derived"})"));
	BOOST_REQUIRE(child);
	BOOST_TEST(child->get_root().children.size() == 1u);
}

BOOST_AUTO_TEST_CASE(SerializeProducesLoadableWorldJson)
{
	rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
	auto world = load<scn::world_desc>(put("w.desc", R"({"__type":"world_desc","name":"w","systems":["s"]})"));
	scn::worldwrap_adapter adapter(*rs, *descs);
	auto bytes = adapter.serialize(tag_of("w.desc"), world);
	std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	auto parsed = boost::json::parse(text).as_object();
	BOOST_TEST(parsed.at("__type").as_string() == "world_desc");
	BOOST_TEST(parsed.at("name").as_string() == "w");
}

BOOST_AUTO_TEST_SUITE_END()
