#include <boost/test/unit_test.hpp>
#include <snk/common/ds/ds_bvh.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <vector>

namespace {

using tree = ds::bvh<int>;

// A box of the given centre and half extent.
ds::aabb3 box_at(const glm::vec3& centre, const glm::vec3& half)
{
	return ds::aabb3{ centre - half, centre + half };
}

struct scene
{
	explicit scene(std::uint32_t seed) : rng(seed) {}

	float real(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
	glm::vec3 point(float extent) { return { real(-extent, extent), real(-extent, extent), real(-extent, extent) }; }

	std::vector<tree::item> items(std::size_t count, float extent = 50.f)
	{
		std::vector<tree::item> out;
		out.reserve(count);
		for (std::size_t i = 0; i < count; ++i) {
			out.push_back({ box_at(point(extent), { real(0.1f, 3.f), real(0.1f, 3.f), real(0.1f, 3.f) }), static_cast<int>(i) });
		}
		return out;
	}

	ds::ray3 ray(float extent = 50.f)
	{
		glm::vec3 direction = point(1.f);
		if (glm::length(direction) < 1e-3f) direction = { 0.f, 0.f, -1.f };
		return ds::ray3{ point(extent), glm::normalize(direction) };
	}

	std::mt19937 rng;
};

std::set<std::uint32_t> brute_ray(const tree& t, const std::vector<tree::item>& items, const ds::ray3& ray, float t_max)
{
	(void)t;
	std::set<std::uint32_t> out;
	for (std::uint32_t id = 0; id < items.size(); ++id) {
		if (ds::ray_aabb(ray, items[id].box, t_max)) out.insert(id);
	}
	return out;
}

std::set<std::uint32_t> bvh_ray_all(const tree& t, const ds::ray3& ray, float t_max)
{
	std::set<std::uint32_t> out;
	t.traverse_ray(ray, t_max, [&out](std::uint32_t id, const int&, float) {
		out.insert(id);
		return ds::bvh_ray_action{ std::numeric_limits<float>::infinity(), false };
	});
	return out;
}

// The nearest entry distance and its id; (inf, none) for a miss.
std::pair<float, std::uint32_t> bvh_closest(const tree& t, const ds::ray3& ray, float t_max)
{
	std::pair<float, std::uint32_t> best{ std::numeric_limits<float>::infinity(), ~0u };
	t.traverse_ray(ray, t_max, [&best](std::uint32_t id, const int&, float entry) {
		if (entry < best.first) best = { entry, id };
		return ds::bvh_ray_action{ best.first, false };
	});
	return best;
}

std::pair<float, std::uint32_t> brute_closest(const std::vector<tree::item>& items, const ds::ray3& ray, float t_max)
{
	std::pair<float, std::uint32_t> best{ std::numeric_limits<float>::infinity(), ~0u };
	for (std::uint32_t id = 0; id < items.size(); ++id) {
		const auto entry = ds::ray_aabb(ray, items[id].box, t_max);
		if (entry && *entry < best.first) best = { *entry, id };
	}
	return best;
}

}

BOOST_AUTO_TEST_SUITE(DsBvhTests)

BOOST_AUTO_TEST_CASE(AnEmptyTree_VisitsNothing_AndHasNoBounds)
{
	tree t;
	BOOST_TEST(t.empty());
	BOOST_TEST(t.size() == 0u);
	BOOST_TEST(t.bounds().empty());
	int calls = 0;
	t.traverse_ray(ds::ray3{ { 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f } }, 100.f, [&calls](std::uint32_t, const int&, float) {
		++calls;
		return ds::bvh_ray_action{};
	});
	t.query_box(box_at({ 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f }), [&calls](std::uint32_t, const int&) { ++calls; return true; });
	t.query_frustum(ds::frustum{}, [&calls](std::uint32_t, const int&, bool) { ++calls; return true; });
	BOOST_TEST(calls == 0);
	BOOST_TEST(t.degradation() == 1.f);
}

BOOST_AUTO_TEST_CASE(AOneItemTree_IsALeaf_AndIsFound)
{
	tree t(std::vector<tree::item>{ { box_at({ 0.f, 0.f, -5.f }, { 1.f, 1.f, 1.f }), 7 } });
	BOOST_TEST(t.size() == 1u);
	const auto hit = bvh_closest(t, ds::ray3{ { 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f } }, 100.f);
	BOOST_TEST(hit.second == 0u);
	BOOST_TEST(hit.first == 4.f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(t.at(0).value == 7);
	BOOST_TEST((t.bounds() == box_at({ 0.f, 0.f, -5.f }, { 1.f, 1.f, 1.f })));
}

BOOST_AUTO_TEST_CASE(RayWalk_FindsExactlyTheBoxesABruteForceDoes_ForRandomSceneAndRays)
{
	scene world(12345);
	const auto items = world.items(2000);
	const tree t(items);
	for (int i = 0; i < 300; ++i) {
		const ds::ray3 ray = world.ray();
		const float t_max = i % 3 == 0 ? std::numeric_limits<float>::infinity() : world.real(5.f, 120.f);
		BOOST_REQUIRE(bvh_ray_all(t, ray, t_max) == brute_ray(t, items, ray, t_max));
	}
}

BOOST_AUTO_TEST_CASE(ClosestRayWalk_FindsTheNearestEntry_OfABruteForce)
{
	scene world(777);
	const auto items = world.items(1500);
	const tree t(items);
	int hits = 0;
	for (int i = 0; i < 300; ++i) {
		const ds::ray3 ray = world.ray();
		const auto expected = brute_closest(items, ray, std::numeric_limits<float>::infinity());
		const auto found = bvh_closest(t, ray, std::numeric_limits<float>::infinity());
		BOOST_REQUIRE(found.first == expected.first);
		if (expected.second != ~0u) ++hits;
	}
	BOOST_TEST(hits > 30, "the random rays must hit something, or the test proves nothing");
}

BOOST_AUTO_TEST_CASE(ClosestRayWalk_PrunesWhatIsFartherThanTheHit)
{
	std::vector<tree::item> line;
	for (int i = 0; i < 2000; ++i) line.push_back({ box_at({ 0.f, 0.f, -2.f * static_cast<float>(i + 1) }, { 0.5f, 0.5f, 0.5f }), i });
	const tree t(line);
	int visited = 0;
	float nearest = std::numeric_limits<float>::infinity();
	t.traverse_ray(ds::ray3{ { 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f } }, std::numeric_limits<float>::infinity(),
		[&](std::uint32_t, const int&, float entry) {
			++visited;
			nearest = std::min(nearest, entry);
			return ds::bvh_ray_action{ nearest, false };
		});
	BOOST_TEST(nearest == 1.5f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(visited < 100, "the walk must go near-first and cut the far boxes off, not visit all 2000: " << visited);
}

BOOST_AUTO_TEST_CASE(ARayVisitor_MayStopTheWalkAtOnce)
{
	scene world(5);
	const auto items = world.items(500, 10.f);
	const tree t(items);
	int calls = 0;
	t.traverse_ray(ds::ray3{ { 0.f, 0.f, 100.f }, { 0.f, 0.f, -1.f } }, std::numeric_limits<float>::infinity(),
		[&calls](std::uint32_t, const int&, float) {
			++calls;
			return ds::bvh_ray_action{ std::numeric_limits<float>::infinity(), true };
		});
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(BoxQuery_FindsExactlyTheIntersectingBoxes)
{
	scene world(99);
	const auto items = world.items(2000);
	const tree t(items);
	for (int i = 0; i < 200; ++i) {
		const ds::aabb3 query = box_at(world.point(50.f), { world.real(1.f, 20.f), world.real(1.f, 20.f), world.real(1.f, 20.f) });
		std::set<std::uint32_t> expected;
		for (std::uint32_t id = 0; id < items.size(); ++id) {
			if (ds::intersects(items[id].box, query)) expected.insert(id);
		}
		std::set<std::uint32_t> found;
		t.query_box(query, [&found](std::uint32_t id, const int&) { found.insert(id); return true; });
		BOOST_REQUIRE(found == expected);
	}
}

BOOST_AUTO_TEST_CASE(BoxQuery_StopsWhenTheVisitorSaysSo)
{
	scene world(3);
	const tree t(world.items(300, 5.f));
	int calls = 0;
	t.query_box(box_at({ 0.f, 0.f, 0.f }, { 100.f, 100.f, 100.f }), [&calls](std::uint32_t, const int&) { return ++calls < 3; });
	BOOST_TEST(calls == 3);
}

BOOST_AUTO_TEST_CASE(FrustumQuery_FindsTheBoxesInsideOrCrossing_AndTellsTheOnesFullyInside)
{
	scene world(2024);
	const auto items = world.items(2000);
	const tree t(items);
	const glm::mat4 view = glm::lookAt(glm::vec3{ 0.f, 0.f, 60.f }, glm::vec3{ 0.f, 0.f, 0.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(60.f), 1.5f, 1.f, 90.f);
	const ds::frustum frustum = ds::frustum_from(projection * view);
	std::set<std::uint32_t> expected;
	std::set<std::uint32_t> expected_inside;
	for (std::uint32_t id = 0; id < items.size(); ++id) {
		const ds::frustum_test test = ds::test_frustum(frustum, items[id].box);
		if (test != ds::frustum_test::outside) expected.insert(id);
		if (test == ds::frustum_test::inside) expected_inside.insert(id);
	}
	BOOST_REQUIRE(!expected.empty());
	BOOST_REQUIRE(expected.size() < items.size());
	std::set<std::uint32_t> found;
	std::set<std::uint32_t> found_inside;
	t.query_frustum(frustum, [&](std::uint32_t id, const int&, bool fully_inside) {
		found.insert(id);
		if (fully_inside) found_inside.insert(id);
		return true;
	});
	BOOST_TEST(found == expected);
	BOOST_TEST(found_inside == expected_inside);
}

BOOST_AUTO_TEST_CASE(SetBox_RefitsTheTree_SoQueriesAgreeWithABruteForceAndARebuild)
{
	scene world(31337);
	auto items = world.items(1500);
	tree t(items);
	for (int frame = 0; frame < 5; ++frame) {
		for (int n = 0; n < 300; ++n) {
			const std::uint32_t id = static_cast<std::uint32_t>(world.real(0.f, static_cast<float>(items.size()) - 0.001f));
			const glm::vec3 shift = world.point(15.f);
			items[id].box = ds::aabb3{ items[id].box.min + shift, items[id].box.max + shift };
			t.set_box(id, items[id].box);
		}
		const tree rebuilt(items);
		for (int i = 0; i < 100; ++i) {
			const ds::ray3 ray = world.ray();
			BOOST_REQUIRE(bvh_ray_all(t, ray, std::numeric_limits<float>::infinity()) == brute_ray(t, items, ray, std::numeric_limits<float>::infinity()));
			BOOST_REQUIRE(bvh_closest(t, ray, 1000.f).first == bvh_closest(rebuilt, ray, 1000.f).first);
		}
		ds::aabb3 everything;
		for (const auto& item : items) ds::expand(everything, item.box);
		BOOST_REQUIRE(t.bounds() == everything);
	}
}

BOOST_AUTO_TEST_CASE(SetBox_ToAnEmptyBox_TakesTheItemOutOfEveryQuery_AndBackAgain)
{
	scene world(8);
	auto items = world.items(200, 10.f);
	tree t(items);
	const ds::ray3 ray{ items[17].box.min - glm::vec3{ 0.f, 0.f, 5.f } + glm::vec3{ 0.01f, 0.01f, 0.f }, { 0.f, 0.f, 1.f } };
	BOOST_REQUIRE(bvh_ray_all(t, ray, 100.f).contains(17u));
	t.set_box(17, ds::aabb3{});
	BOOST_TEST(!bvh_ray_all(t, ray, 100.f).contains(17u));
	t.set_box(17, items[17].box);
	BOOST_TEST(bvh_ray_all(t, ray, 100.f).contains(17u));
}

BOOST_AUTO_TEST_CASE(Degradation_GrowsWhenItemsScatter_AndOneAgainAfterARebuild)
{
	scene world(11);
	auto items = world.items(1000, 20.f);
	tree t(items);
	BOOST_TEST(t.degradation() == 1.f);
	for (std::uint32_t id = 0; id < items.size(); ++id) {
		const glm::vec3 shift = world.point(200.f);
		t.set_box(id, ds::aabb3{ items[id].box.min + shift, items[id].box.max + shift });
	}
	BOOST_TEST(t.degradation() > 1.5f);
	t.rebuild();
	BOOST_TEST(t.degradation() == 1.f);
	BOOST_TEST(t.size() == items.size());
}

BOOST_AUTO_TEST_CASE(ManyItemsWithTheSameCentre_CannotBeSplit_ButAreAllFound)
{
	std::vector<tree::item> same;
	for (int i = 0; i < 100; ++i) same.push_back({ box_at({ 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f }), i });
	const tree t(same);
	BOOST_TEST(bvh_ray_all(t, ds::ray3{ { 1.f, 1.f, 10.f }, { 0.f, 0.f, -1.f } }, 100.f).size() == 100u);
}

BOOST_AUTO_TEST_CASE(EmptyBoxesAmongTheItems_NeverHit_AndDoNotBreakTheBuild)
{
	std::vector<tree::item> items;
	for (int i = 0; i < 40; ++i) items.push_back({ i % 2 == 0 ? ds::aabb3{} : box_at({ static_cast<float>(i), 0.f, 0.f }, { 0.4f, 0.4f, 0.4f }), i });
	const tree t(items);
	const auto found = bvh_ray_all(t, ds::ray3{ { -5.f, 0.f, 0.f }, { 1.f, 0.f, 0.f } }, 1000.f);
	BOOST_TEST(found.size() == 20u);
	for (const std::uint32_t id : found) BOOST_TEST(id % 2 == 1u);
}

BOOST_AUTO_TEST_CASE(ABigTree_BuildsAndAnswers_WithoutOverflowingItsStack)
{
	scene world(424242);
	const auto items = world.items(100000, 500.f);
	const tree t(items);
	for (int i = 0; i < 20; ++i) {
		const ds::ray3 ray = world.ray(500.f);
		BOOST_REQUIRE(bvh_closest(t, ray, 2000.f).first == brute_closest(items, ray, 2000.f).first);
	}
}

BOOST_AUTO_TEST_SUITE_END()
