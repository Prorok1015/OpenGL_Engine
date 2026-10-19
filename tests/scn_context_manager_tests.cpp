#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_primitives.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/adapters/scn_worldwrap_adapter.h>
#include <cmath>
#include <set>
#include <tuple>

// Procedural primitives (pure math), context_manager load/hot-reload policy and the world wrapper
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

// ---------------------------------------------------------------- context_manager + worldwrap_adapter

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
	scn::context_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnContextManagerTests, manager_fixture)

BOOST_AUTO_TEST_CASE(Load_BuildsTheLevelFromADescTag)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_TEST(mgr.load_level(tag));
	auto& lvl = mgr.level_set();
	BOOST_TEST(lvl.size() == 2u);
	BOOST_TEST(lvl.tag() == tag);
	BOOST_TEST(has_named(lvl.get("w0").state(), "c1"));
}

BOOST_AUTO_TEST_CASE(Load_MissingLevelReturnsFalseAndKeepsCurrentLevel)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", LEVEL_V1)));
	BOOST_TEST(!mgr.load_level(tag_of("does_not_exist.desc")));
	BOOST_TEST(mgr.level_set().size() == 2u);
	BOOST_TEST(has_named(mgr.level_set().get("w0").state(), "c1"));
}

BOOST_AUTO_TEST_CASE(Load_SecondLevelReplacesTheFirst)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", LEVEL_V1)));
	BOOST_REQUIRE(mgr.load_level(put("lv3.desc", LEVEL_V3_DIFFERENT_WORLDS)));
	auto& lvl = mgr.level_set();
	BOOST_TEST(lvl.size() == 1u);
	BOOST_TEST(lvl.has("only"));
	BOOST_TEST(!lvl.has("w0"));
}

BOOST_AUTO_TEST_CASE(HotReload_SameWorldNamesOnlyReloadSystemsNotContent)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));

	put("lv.desc", LEVEL_V2_SAME_NAMES);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	auto& reg = mgr.level_set().get("w0").state();
	BOOST_TEST(has_named(reg, "c1"));    // existing entities are kept...
	BOOST_TEST(!has_named(reg, "c2"));   // ...the new content is NOT spawned (only systems are re-read)
}

BOOST_AUTO_TEST_CASE(HotReload_ChangedWorldListTriggersFullReload)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));

	put("lv.desc", LEVEL_V3_DIFFERENT_WORLDS);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	auto& lvl = mgr.level_set();
	BOOST_TEST(lvl.size() == 1u);
	BOOST_TEST(lvl.has("only"));
	BOOST_TEST(has_named(lvl.get("only").state(), "c3"));
}

BOOST_AUTO_TEST_CASE(HotReload_FullReloadKeepsRuntimeCreatedWorlds)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	auto& lvl = mgr.level_set();
	auto& rt_reg = lvl.create_context("rt", lvl.acquire_free_id()).state();
	rt_reg.emplace<scn::name_component>(rt_reg.create(), scn::name_component{ "rt_ent" });

	int reloads = 0;
	mgr.set_on_reload([&reloads] { ++reloads; });
	put("lv.desc", LEVEL_V3_DIFFERENT_WORLDS);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(reloads == 1);
	BOOST_TEST(lvl.size() == 2u);
	BOOST_TEST(lvl.has("only"));
	BOOST_TEST(!lvl.has("w0"));
	BOOST_REQUIRE(lvl.has("rt"));
	BOOST_TEST(has_named(lvl.get("rt").state(), "rt_ent"));
	BOOST_TEST(lvl.get("rt").get_id() != lvl.get("only").get_id());
}

BOOST_AUTO_TEST_CASE(HotReload_SystemsOnlyReloadKeepsRuntimeCreatedWorlds)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	auto& lvl = mgr.level_set();
	lvl.create_context("rt", lvl.acquire_free_id());

	int reloads = 0;
	mgr.set_on_reload([&reloads] { ++reloads; });
	put("lv.desc", LEVEL_V2_SAME_NAMES);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(reloads == 1); // the editor resyncs its system lists on a systems-only reload too
	BOOST_TEST(lvl.size() == 3u);
	BOOST_TEST(has_named(lvl.get("w0").state(), "c1")); // an extra runtime world must not turn this into a full reload
}

BOOST_AUTO_TEST_CASE(HotReload_FailedWorldDescKeepsTheCurrentLevelAndSkipsTheCallback)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	auto& lvl = mgr.level_set();
	lvl.create_context("rt", lvl.acquire_free_id());

	int reloads = 0;
	mgr.set_on_reload([&reloads] { ++reloads; });
	// the second world is a tag of a file that does not exist
	put("lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":[]},"memory://t/missing_world.desc"]})");
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(reloads == 0);
	BOOST_TEST(lvl.size() == 3u);
	BOOST_TEST(lvl.has("w0"));
	BOOST_TEST(lvl.has("w1"));
	BOOST_TEST(lvl.has("rt"));
	BOOST_TEST(has_named(lvl.get("w0").state(), "c1")); // not rebuilt
}

BOOST_AUTO_TEST_CASE(HotReload_DescWorldReplacesRuntimeWorldWithTheSameName)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	auto& lvl = mgr.level_set();
	lvl.create_context("only", lvl.acquire_free_id());

	put("lv.desc", LEVEL_V3_DIFFERENT_WORLDS);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(lvl.size() == 1u);
	BOOST_TEST(has_named(lvl.get("only").state(), "c3"));
}

BOOST_AUTO_TEST_CASE(HotReload_ReloadingAnotherLevelStopsWatchingTheOldOne)
{
	auto tag1 = put("lv.desc", LEVEL_V1);
	auto tag3 = put("lv3.desc", LEVEL_V3_DIFFERENT_WORLDS);
	BOOST_REQUIRE(mgr.load_level(tag1));
	BOOST_REQUIRE(mgr.load_level(tag3));

	// the first level's file changes afterwards: must not tear down the active level
	put("lv.desc", LEVEL_V2_SAME_NAMES);
	rs->signal_changed(tag1);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));
	BOOST_TEST(mgr.level_set().has("only"));
	BOOST_TEST(mgr.level_set().size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- context sets

BOOST_FIXTURE_TEST_SUITE(ScnContextSetsTests, manager_fixture)

namespace {

std::vector<std::string> names_in(scn::context_manager& mgr, std::string_view set)
{
	std::vector<std::string> out;
	mgr.for_each(set, [&out](scn::world_context& c) { out.emplace_back(c.get_name()); });
	return out;
}

float delta_of(scn::world_context& c) { return c.state().ctx().get<scn::delta_time>().dt; }

}

BOOST_AUTO_TEST_CASE(LevelSet_ExistsFromTheStartAndIsTheFacadeTarget)
{
	BOOST_REQUIRE(mgr.find_set("level"));
	BOOST_TEST(mgr.find_set("level")->name() == "level");
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", LEVEL_V1)));

	auto* w0 = mgr.find("level", "w0");
	BOOST_REQUIRE(w0);
	BOOST_TEST(w0 == &mgr.level_set().get("w0"));
	BOOST_TEST((w0->kind() == scn::context_kind::level));
	BOOST_TEST(mgr.level_set().tag() == mgr.get_set("level").tag());
}

BOOST_AUTO_TEST_CASE(Sets_AreIsolatedFromEachOther)
{
	mgr.create_set("a");
	mgr.create_set("b");
	auto& in_a = mgr.create_context("a", "x", scn::context_kind::runtime);
	auto& in_b = mgr.create_context("b", "x", scn::context_kind::preview);
	mgr.create_context("b", "y", scn::context_kind::preview);

	BOOST_TEST(&in_a != &in_b);
	BOOST_TEST(mgr.find("a", "x") == &in_a);
	BOOST_TEST(mgr.find("b", "x") == &in_b);
	BOOST_TEST(mgr.find("a", "y") == nullptr);
	BOOST_TEST(mgr.find("nope", "x") == nullptr);
	BOOST_TEST(mgr.get_set("a").size() == 1u);
	BOOST_TEST(mgr.get_set("b").size() == 2u);
	BOOST_TEST(mgr.level_set().size() == 0u);

	mgr.destroy_context("b", "x");
	BOOST_TEST(mgr.find("b", "x") == nullptr);
	BOOST_TEST(mgr.find("a", "x") == &in_a);

	mgr.destroy_set("b");
	BOOST_TEST(mgr.find_set("b") == nullptr);
	BOOST_TEST(mgr.find("a", "x") == &in_a);
}

BOOST_AUTO_TEST_CASE(CreateSet_WithATakenNameKeepsTheExistingSet)
{
	auto& first = mgr.create_set("a");
	mgr.create_context("a", "x", scn::context_kind::runtime);
	BOOST_TEST(&mgr.create_set("a") == &first);
	BOOST_TEST(first.size() == 1u);
}

BOOST_AUTO_TEST_CASE(ForEach_WalksASetInIdOrderAndAllSetsInCreationOrder)
{
	mgr.create_set("a");
	mgr.create_set("b");
	mgr.create_context("a", "c", scn::context_kind::runtime);
	mgr.create_context("a", "a", scn::context_kind::runtime);
	mgr.create_context("a", "b", scn::context_kind::runtime);
	mgr.create_context("b", "z", scn::context_kind::runtime);
	BOOST_TEST((names_in(mgr, "a") == std::vector<std::string>{ "c", "a", "b" }));

	// the freed id is reused, so the new context takes the removed one's place
	mgr.destroy_context("a", "a");
	mgr.create_context("a", "d", scn::context_kind::runtime);
	BOOST_TEST((names_in(mgr, "a") == std::vector<std::string>{ "c", "d", "b" }));

	std::vector<std::string> all;
	mgr.for_each([&all](scn::world_context& c) { all.emplace_back(c.get_name()); });
	BOOST_TEST((all == std::vector<std::string>{ "c", "d", "b", "z" }));
}

BOOST_AUTO_TEST_CASE(SetMode_AppliesToOneSetOnly)
{
	mgr.create_set("a");
	mgr.create_set("b");
	auto& in_a = mgr.create_context("a", "x", scn::context_kind::runtime);
	auto& in_b = mgr.create_context("b", "x", scn::context_kind::runtime);

	mgr.set_mode("a", ecs::run_mode::pause);
	BOOST_TEST((in_a.mode() == ecs::run_mode::pause));
	BOOST_TEST((in_b.mode() == ecs::run_mode::play));

	mgr.update(std::chrono::duration<float>(0.01f));
	BOOST_TEST(delta_of(in_a) == 0.0f); // frozen
	BOOST_TEST(delta_of(in_b) > 0.0f);

	// a context created later joins with its set's mode
	BOOST_TEST((mgr.create_context("a", "y", scn::context_kind::runtime).mode() == ecs::run_mode::pause));
	// the level set keeps its own mode
	BOOST_TEST((mgr.level_set().mode() == ecs::run_mode::play));
}

BOOST_AUTO_TEST_CASE(SetSeed_AppliesToOneSetOnly)
{
	mgr.create_set("a");
	mgr.create_set("b");
	auto& in_a = mgr.create_context("a", "x", scn::context_kind::runtime);
	auto& in_b = mgr.create_context("b", "x", scn::context_kind::runtime);

	mgr.set_seed("a", 1234);
	BOOST_TEST(in_a.state().ctx().get<ecs::rng>().seed() == ecs::derive_seed(1234, in_a.get_id()));
	BOOST_TEST(in_b.state().ctx().get<ecs::rng>().seed() == ecs::derive_seed(ecs::rng::default_seed, in_b.get_id()));
	BOOST_TEST(mgr.get_set("a").seed() == 1234u);
	BOOST_TEST(mgr.get_set("b").seed() == ecs::rng::default_seed);
}

BOOST_AUTO_TEST_CASE(Update_SkipsAnInactiveSet)
{
	mgr.create_set("a");
	auto& in_a = mgr.create_context("a", "x", scn::context_kind::runtime);
	mgr.get_set("a").set_active(false);
	mgr.update(std::chrono::duration<float>(0.01f));
	BOOST_TEST(delta_of(in_a) == 0.0f);

	mgr.get_set("a").set_active(true);
	mgr.update(std::chrono::duration<float>(0.01f));
	BOOST_TEST(delta_of(in_a) > 0.0f);
}

BOOST_AUTO_TEST_CASE(Addresses_StayStableAcrossCreateAndDestroy)
{
	mgr.create_set("a");
	auto& first = mgr.create_context("a", "first", scn::context_kind::runtime);
	auto* registry = &first.state();
	mgr.create_context("a", "victim", scn::context_kind::runtime);
	std::vector<scn::world_context*> more;
	for (int i = 0; i < 64; ++i) {
		more.push_back(&mgr.create_context("a", "ctx" + std::to_string(i), scn::context_kind::runtime));
	}
	mgr.destroy_context("a", "victim");
	mgr.create_context("a", "late", scn::context_kind::runtime);

	BOOST_TEST(mgr.find("a", "first") == &first);
	BOOST_TEST(&first.state() == registry);
	for (int i = 0; i < 64; ++i) {
		BOOST_TEST(mgr.find("a", "ctx" + std::to_string(i)) == more[i]);
	}
	// sets keep their address as well
	auto* set_a = &mgr.get_set("a");
	mgr.create_set("b");
	mgr.create_set("c");
	mgr.destroy_set("b");
	BOOST_TEST(&mgr.get_set("a") == set_a);
}

BOOST_AUTO_TEST_CASE(CreateContext_BuildsThePlanFromTheGivenSystems)
{
	mgr.create_set("a");
	auto& context = mgr.create_context("a", "x", scn::context_kind::runtime, {});
	BOOST_TEST(context.plan() != nullptr); // an empty list still gets a plan
	BOOST_TEST(mgr.get_set("a").plan_of(context.get_id()) == context.plan());
}

BOOST_AUTO_TEST_CASE(HotReload_ChangedWorldListTouchesOnlyLevelKindContexts)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));

	auto& rt = mgr.create_context("level", "rt", scn::context_kind::runtime);
	rt.state().emplace<scn::name_component>(rt.state().create(), scn::name_component{ "rt_ent" });
	mgr.create_set("preview:p");
	auto& preview = mgr.create_context("preview:p", "w0", scn::context_kind::preview); // same name as a level world
	preview.state().emplace<scn::name_component>(preview.state().create(), scn::name_component{ "pv_ent" });

	put("lv.desc", LEVEL_V3_DIFFERENT_WORLDS);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(mgr.find("level", "w0") == nullptr);
	BOOST_TEST(mgr.find("level", "w1") == nullptr);
	BOOST_REQUIRE(mgr.find("level", "only"));
	BOOST_TEST((mgr.find("level", "only")->kind() == scn::context_kind::level));
	BOOST_TEST(mgr.find("level", "rt") == &rt);
	BOOST_TEST(has_named(rt.state(), "rt_ent"));
	// the other set is not part of the level: neither compared nor rebuilt
	BOOST_TEST(mgr.find("preview:p", "w0") == &preview);
	BOOST_TEST(has_named(preview.state(), "pv_ent"));
	BOOST_TEST(mgr.get_set("preview:p").size() == 1u);
}

BOOST_AUTO_TEST_CASE(HotReload_ReorderedWorldsForceAFullReloadSoIdsFollowTheDesc)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	BOOST_REQUIRE(mgr.find("level", "w0")->get_id() < mgr.find("level", "w1")->get_id());

	put("lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w1","systems":[]},
		{"__type":"world_desc","name":"w0","systems":[],"children":{"c2":{"components":{"stat":{"__type":"stat_desc","hp":2}}}}}]})");
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(has_named(mgr.level_set().get("w0").state(), "c2")); // rebuilt, not only systems
	BOOST_TEST(mgr.find("level", "w1")->get_id() < mgr.find("level", "w0")->get_id());
}

BOOST_AUTO_TEST_CASE(HotReload_SystemsOnlyReloadIgnoresContextsOfOtherKinds)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	mgr.create_context("level", "rt", scn::context_kind::runtime);
	mgr.create_set("preview:p");
	mgr.create_context("preview:p", "pv", scn::context_kind::preview);

	int reloads = 0;
	mgr.set_on_reload([&reloads] { ++reloads; });
	put("lv.desc", LEVEL_V2_SAME_NAMES);
	rs->signal_changed(tag);
	pump();
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(reloads == 1);
	BOOST_TEST(has_named(mgr.level_set().get("w0").state(), "c1")); // not a full reload
	BOOST_TEST(mgr.level_set().size() == 3u);
	BOOST_TEST(mgr.get_set("preview:p").size() == 1u);
}

// ---------------------------------------------------------------- previews (#112)

BOOST_AUTO_TEST_CASE(Preview_IsASetOfItsOwnWithOnePreviewContextInEditMode)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", LEVEL_V1)));
	auto& preview = mgr.create_preview("res://a.prefab.desc", "pv");

	BOOST_TEST(scn::context_manager::preview_set_name("res://a.prefab.desc") == "preview:res://a.prefab.desc");
	auto* set = mgr.find_set("preview:res://a.prefab.desc");
	BOOST_REQUIRE(set);
	BOOST_TEST(set->size() == 1u);
	BOOST_TEST(set->active());
	BOOST_TEST((set->mode() == ecs::run_mode::edit));
	BOOST_TEST((preview.kind() == scn::context_kind::preview));
	BOOST_TEST((preview.mode() == ecs::run_mode::edit));
	BOOST_TEST(preview.extract());
	BOOST_TEST(mgr.find_preview("res://a.prefab.desc") == &preview);
	BOOST_TEST(mgr.find("preview:res://a.prefab.desc", "pv") == &preview);
	// the level did not get a context
	BOOST_TEST(mgr.find("level", "pv") == nullptr);
	BOOST_TEST(mgr.level_set().size() == 2u);
	BOOST_TEST((mgr.level_set().mode() == ecs::run_mode::play)); // the preview's mode is its own
}

BOOST_AUTO_TEST_CASE(Preview_HasItsOwnTime)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", LEVEL_V1)));
	auto& preview = mgr.create_preview("p", "pv");
	auto& level_world = mgr.level_set().get("w0");

	mgr.set_mode("level", ecs::run_mode::pause);
	mgr.update(std::chrono::duration<float>(0.01f));
	BOOST_TEST(delta_of(level_world) == 0.0f); // frozen
	BOOST_TEST(delta_of(preview) > 0.0f);      // not affected by the level
	BOOST_TEST((preview.mode() == ecs::run_mode::edit));
}

BOOST_AUTO_TEST_CASE(Preview_DestroyRemovesTheSetAndReopeningStartsClean)
{
	auto& first = mgr.create_preview("p", "pv");
	first.state().emplace<scn::name_component>(first.state().create(), scn::name_component{ "pv_ent" });
	BOOST_REQUIRE(mgr.find_preview("p"));

	mgr.destroy_preview("p");
	BOOST_TEST(mgr.find_set("preview:p") == nullptr);
	BOOST_TEST(mgr.find_preview("p") == nullptr);
	BOOST_TEST(mgr.find_set("level") != nullptr);
	mgr.destroy_preview("p"); // nothing to destroy: not an error

	auto& again = mgr.create_preview("p", "pv");
	BOOST_TEST(!has_named(again.state(), "pv_ent"));
	BOOST_TEST(mgr.get_set("preview:p").size() == 1u);
}

BOOST_AUTO_TEST_CASE(Preview_CreatingForTheSameTagReplacesThePreviousOne)
{
	auto& first = mgr.create_preview("p", "pv");
	first.state().emplace<scn::name_component>(first.state().create(), scn::name_component{ "old" });

	auto& second = mgr.create_preview("p", "pv");

	BOOST_TEST(mgr.find_preview("p") == &second);
	BOOST_TEST(!has_named(second.state(), "old"));
	std::size_t previews = 0;
	mgr.for_each_set([&previews](scn::context_set& set) {
		if (set.name().starts_with(scn::context_manager::preview_set_prefix)) ++previews;
	});
	BOOST_TEST(previews == 1u);
}

BOOST_AUTO_TEST_CASE(Preview_TagsDoNotShareASet)
{
	auto& a = mgr.create_preview("a", "pv");
	auto& b = mgr.create_preview("b", "pv"); // same context name in another set

	BOOST_TEST(&a != &b);
	BOOST_TEST(mgr.find_preview("a") == &a);
	BOOST_TEST(mgr.find_preview("b") == &b);

	mgr.destroy_preview("a");
	BOOST_TEST(mgr.find_preview("a") == nullptr);
	BOOST_TEST(mgr.find_preview("b") == &b);
}

BOOST_AUTO_TEST_CASE(Preview_LevelReloadKeepsItIntactEvenWithANameOfTheLevel)
{
	auto tag = put("lv.desc", LEVEL_V1);
	BOOST_REQUIRE(mgr.load_level(tag));
	auto& preview = mgr.create_preview("p", "w0"); // the name of a level world
	preview.state().emplace<scn::name_component>(preview.state().create(), scn::name_component{ "pv_ent" });
	auto* registry = &preview.state();

	for (const char* next : { LEVEL_V3_DIFFERENT_WORLDS, LEVEL_V1 }) { // twice, the second one brings "w0" back
		put("lv.desc", next);
		rs->signal_changed(tag);
		pump();
		mgr.update(std::chrono::duration<float>(0.001f));

		BOOST_TEST(mgr.find_preview("p") == &preview);
		BOOST_TEST(&preview.state() == registry);
		BOOST_TEST(has_named(preview.state(), "pv_ent"));
		BOOST_TEST(mgr.get_set("preview:p").size() == 1u);
	}
	BOOST_REQUIRE(mgr.find("level", "w0"));
	BOOST_TEST(mgr.find("level", "w0") != &preview);
	BOOST_TEST((mgr.find("level", "w0")->kind() == scn::context_kind::level));
}

BOOST_AUTO_TEST_CASE(Preview_TheLevelSetNeverContainsIt)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", LEVEL_V1)));
	mgr.create_preview("p", "pv");

	mgr.get_set("level").for_each([](scn::world_context& context) {
		BOOST_TEST((context.kind() != scn::context_kind::preview));
	});
	BOOST_TEST(mgr.level_set().size() == 2u);
	BOOST_TEST(mgr.get_set("level").desc_world_names().size() == 2u);
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
