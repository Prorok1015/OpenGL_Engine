#include <boost/test/unit_test.hpp>
#include <snk/common/ds/ds_bvh.hpp>
#include <snk/common/ds/ds_shape.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace {

struct triangle
{
	glm::vec3 a;
	glm::vec3 b;
	glm::vec3 c;
};

struct generator
{
	explicit generator(std::uint32_t seed) : rng(seed) {}

	float real(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
	glm::vec3 point(float extent) { return { real(-extent, extent), real(-extent, extent), real(-extent, extent) }; }

	triangle make_triangle(float extent = 1.5f) { return { point(extent), point(extent), point(extent) }; }

	glm::vec3 unit()
	{
		glm::vec3 v = point(1.f);
		return glm::length(v) < 1e-2f ? glm::vec3{ 0.f, 1.f, 0.f } : glm::normalize(v);
	}

	std::mt19937 rng;
};

// Every point of a barycentric grid of the triangle: what the nearest point is checked against (an upper bound of the true distance,
// off by at most a grid step).
template <class Visit>
void for_each_sample(const triangle& t, int steps, Visit&& visit)
{
	for (int i = 0; i <= steps; ++i) {
		for (int j = 0; i + j <= steps; ++j) {
			const float u = static_cast<float>(i) / static_cast<float>(steps);
			const float v = static_cast<float>(j) / static_cast<float>(steps);
			visit(t.a + (t.b - t.a) * u + (t.c - t.a) * v);
		}
	}
}

float sampled_distance(const glm::vec3& p, const triangle& t, int steps)
{
	float best = std::numeric_limits<float>::max();
	for_each_sample(t, steps, [&](const glm::vec3& s) { best = std::min(best, glm::distance(p, s)); });
	return best;
}

constexpr float k_grid_slack = 0.15f;

}

BOOST_AUTO_TEST_SUITE(DsShape)

BOOST_AUTO_TEST_CASE(TheBoundsOfAShape_HoldItsExtremes)
{
	const ds::aabb3 sphere = ds::bounds_of(ds::sphere3{ { 1.f, 2.f, 3.f }, 0.5f });
	BOOST_TEST((sphere.min == glm::vec3{ 0.5f, 1.5f, 2.5f }));
	BOOST_TEST((sphere.max == glm::vec3{ 1.5f, 2.5f, 3.5f }));

	const ds::aabb3 capsule = ds::bounds_of(ds::capsule3{ { 0.f, 0.f, 0.f }, { 0.f, 4.f, 1.f }, 1.f });
	BOOST_TEST((capsule.min == glm::vec3{ -1.f, -1.f, -1.f }));
	BOOST_TEST((capsule.max == glm::vec3{ 1.f, 5.f, 2.f }));

	const glm::mat3 turned = glm::mat3_cast(glm::angleAxis(glm::radians(45.f), glm::vec3{ 0.f, 0.f, 1.f }));
	const ds::aabb3 box = ds::bounds_of(ds::obb3{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 2.f }, turned });
	BOOST_TEST(box.max.x == std::sqrt(2.f), boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(box.max.y == std::sqrt(2.f), boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(box.max.z == 2.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(TheNearestPointOfATriangle_MatchesAGridOfItsPoints)
{
	generator gen(11);
	for (int run = 0; run < 150; ++run) {
		const triangle t = gen.make_triangle();
		const glm::vec3 p = gen.point(5.f);
		const glm::vec3 nearest = ds::closest_point_on_triangle(p, t.a, t.b, t.c);
		const float sampled = sampled_distance(p, t, 80);
		const float exact = glm::distance(p, nearest);
		BOOST_TEST_CONTEXT("run " << run) {
			BOOST_TEST(exact <= sampled + 1e-4f);
			BOOST_TEST(exact >= sampled - 0.1f);
		}
	}
}

BOOST_AUTO_TEST_CASE(ADegenerateTriangle_HasANearestPointAndNoNaN)
{
	const glm::vec3 p{ 1.f, 1.f, 1.f };
	const glm::vec3 point = ds::closest_point_on_triangle(p, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f });
	BOOST_TEST(!std::isnan(point.x + point.y + point.z));
	const glm::vec3 line = ds::closest_point_on_triangle({ 1.f, 1.f, 0.f }, { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { 4.f, 0.f, 0.f });
	BOOST_TEST(line.y == 0.f);
	BOOST_TEST(line.x == 1.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(SphereAndCapsule_MeetATriangle_WhereAGridOfItsPointsSaysSo)
{
	generator gen(21);
	for (int run = 0; run < 250; ++run) {
		const triangle t = gen.make_triangle();
		const glm::vec3 a = gen.point(4.f);
		const glm::vec3 b = a + gen.unit() * gen.real(0.f, 3.f);
		const float radius = gen.real(0.2f, 1.5f);
		const float point_gap = sampled_distance(a, t, 60);
		float segment_gap = std::numeric_limits<float>::max();
		for (int k = 0; k <= 60; ++k) {
			const glm::vec3 on_segment = a + (b - a) * (static_cast<float>(k) / 60.f);
			segment_gap = std::min(segment_gap, sampled_distance(on_segment, t, 40));
		}
		BOOST_TEST_CONTEXT("run " << run) {
			if (point_gap < radius - k_grid_slack) BOOST_TEST(ds::sphere_triangle({ a, radius }, t.a, t.b, t.c));
			if (point_gap > radius + k_grid_slack) BOOST_TEST(!ds::sphere_triangle({ a, radius }, t.a, t.b, t.c));
			if (segment_gap < radius - k_grid_slack) BOOST_TEST(ds::capsule_triangle({ a, b, radius }, t.a, t.b, t.c));
			if (segment_gap > radius + k_grid_slack) BOOST_TEST(!ds::capsule_triangle({ a, b, radius }, t.a, t.b, t.c));
		}
	}
}

BOOST_AUTO_TEST_CASE(ACapsuleThatPiercesATriangle_MeetsItWithNoRadius)
{
	const triangle t{ { -1.f, 0.f, -1.f }, { 1.f, 0.f, -1.f }, { 0.f, 0.f, 1.f } };
	BOOST_TEST(ds::capsule_triangle({ { 0.f, -1.f, 0.f }, { 0.f, 1.f, 0.f }, 0.f }, t.a, t.b, t.c));
	BOOST_TEST(!ds::capsule_triangle({ { 5.f, -1.f, 0.f }, { 5.f, 1.f, 0.f }, 0.1f }, t.a, t.b, t.c));
	BOOST_TEST(ds::capsule_triangle({ { 0.f, 1.f, 0.f }, { 0.f, 1.f, 0.f }, 1.f }, t.a, t.b, t.c));
}

BOOST_AUTO_TEST_CASE(ABox_MeetsATriangle_WhereAGridOfItsPointsSaysSo)
{
	generator gen(31);
	for (int run = 0; run < 300; ++run) {
		const triangle t = gen.make_triangle();
		const glm::quat turn = glm::angleAxis(gen.real(0.f, 6.28f), gen.unit());
		const ds::obb3 box{ gen.point(2.f), { gen.real(0.2f, 1.5f), gen.real(0.2f, 1.5f), gen.real(0.2f, 1.5f) }, glm::mat3_cast(turn) };
		const glm::mat3 to_local = glm::transpose(box.axes);
		bool inside = false;
		bool near = false;
		const glm::vec3 slack{ 0.1f };
		for_each_sample(t, 60, [&](const glm::vec3& s) {
			const glm::vec3 local = to_local * (s - box.center);
			if (glm::all(glm::lessThanEqual(glm::abs(local), box.half))) inside = true;
			if (glm::all(glm::lessThanEqual(glm::abs(local), box.half + slack))) near = true;
		});
		BOOST_TEST_CONTEXT("run " << run) {
			if (inside) BOOST_TEST(ds::obb_triangle(box, t.a, t.b, t.c));
			if (!near) BOOST_TEST(!ds::obb_triangle(box, t.a, t.b, t.c));
		}
	}
}

BOOST_AUTO_TEST_CASE(ABoxAndATriangle_ThatOnlyTouchAtAnEdge_Meet)
{
	const ds::obb3 box{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f }, glm::mat3{ 1.f } };
	BOOST_TEST(ds::obb_triangle(box, { 1.f, 1.f, -3.f }, { 1.f, 1.f, 3.f }, { 3.f, 3.f, 0.f }));
	BOOST_TEST(!ds::obb_triangle(box, { 1.2f, 1.2f, -3.f }, { 1.2f, 1.2f, 3.f }, { 3.f, 3.f, 0.f }));
}

BOOST_AUTO_TEST_CASE(ASphereMovingDownAtAFace_StopsAtTheRadius_WithTheFacesNormal)
{
	const triangle t{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const auto hit = ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, -1.f, 0.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->t == 4.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->point.y == 0.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->normal.y == 1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(!hit->started_inside);
}

BOOST_AUTO_TEST_CASE(ASphereFromBelow_GetsTheNormalPointingDown)
{
	const triangle t{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const auto hit = ds::sphere_sweep_triangle({ { 0.f, -5.f, 0.f }, 1.f }, { 0.f, 2.f, 0.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->t == 2.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->normal.y == -1.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(ASphereGrazingAnEdgeAndACorner_HitsThemAtTheRadiusFromThem)
{
	const triangle t{ { 0.f, 0.f, 0.f }, { 4.f, 0.f, 0.f }, { 0.f, 0.f, 4.f } };
	// Moving along z at the edge a-b (z = 0): the centre stops a radius short of it.
	const auto edge = ds::sphere_sweep_triangle({ { 2.f, 0.f, -5.f }, 1.f }, { 0.f, 0.f, 1.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(edge.has_value());
	BOOST_TEST(edge->t == 4.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(edge->normal.z == -1.f, boost::test_tools::tolerance(1e-4f));

	const auto corner = ds::sphere_sweep_triangle({ { -5.f, 0.f, 0.f }, 1.f }, { 1.f, 0.f, 0.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(corner.has_value());
	BOOST_TEST(corner->t == 4.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(corner->point.x == 0.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(ASphereThatStartsInsideOrMissesOrIsTooShort_IsTold)
{
	const triangle t{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const auto inside = ds::sphere_sweep_triangle({ { 0.f, 0.5f, 0.f }, 1.f }, { 0.f, 1.f, 0.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(inside.has_value());
	BOOST_TEST(inside->started_inside);
	BOOST_TEST(inside->t == 0.f);
	BOOST_TEST(inside->normal.y == 1.f, boost::test_tools::tolerance(1e-4f));

	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, 1.f, 0.f }, t.a, t.b, t.c).has_value());
	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 1.f, 0.f, 0.f }, t.a, t.b, t.c).has_value());
	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, -1.f, 0.f }, t.a, t.b, t.c, 3.f).has_value());
	BOOST_TEST(ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, -1.f, 0.f }, t.a, t.b, t.c, 4.f).has_value());
	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, 0.f, 0.f }, t.a, t.b, t.c).has_value());
}

BOOST_AUTO_TEST_CASE(TheSweepOfASphere_StopsWhereItTouches_AndNothingBeforeItDoes)
{
	generator gen(41);
	constexpr float k_reach = 8.f;
	constexpr float k_step = 0.01f;
	// The independent oracle marches in coarser steps and asks the grid of the triangle's points, which never reads the code under
	// test: a grid distance is never below the true one, so one clearly under the radius is a certain overlap.
	constexpr float k_grid_step = 0.02f;
	int hits = 0;
	for (int run = 0; run < 300; ++run) {
		const triangle t = gen.make_triangle();
		const ds::sphere3 sphere{ gen.point(3.f), gen.real(0.2f, 1.2f) };
		const glm::vec3 direction = gen.unit();
		const auto hit = ds::sphere_sweep_triangle(sphere, direction, t.a, t.b, t.c, k_reach);
		std::optional<float> marched;
		for (float at = 0.f; at <= k_reach; at += k_step) {
			if (ds::sphere_triangle({ sphere.center + direction * at, sphere.radius }, t.a, t.b, t.c)) {
				marched = at;
				break;
			}
		}
		std::optional<float> certain;
		for (float at = 0.f; at <= k_reach; at += k_grid_step) {
			if (sampled_distance(sphere.center + direction * at, t, 30) < sphere.radius - k_grid_slack) {
				certain = at;
				break;
			}
		}
		BOOST_TEST_CONTEXT("run " << run) {
			if (certain) {
				BOOST_REQUIRE(hit.has_value());
				BOOST_TEST(hit->t <= *certain + 1e-3f);
			}
			if (marched) {
				BOOST_REQUIRE(hit.has_value());
				BOOST_TEST(hit->t <= *marched + 1e-3f);
				BOOST_TEST(hit->t >= *marched - k_step - 1e-3f);
			}
			if (hit && !hit->started_inside) {
				++hits;
				const glm::vec3 at = sphere.center + direction * hit->t;
				const float gap = glm::distance(at, ds::closest_point_on_triangle(at, t.a, t.b, t.c));
				BOOST_TEST(gap == sphere.radius, boost::test_tools::tolerance(2e-3f));
				BOOST_TEST(sampled_distance(at, t, 30) >= sphere.radius - 1e-3f);
				BOOST_TEST(ds::sphere_triangle({ at, sphere.radius + 2e-3f }, t.a, t.b, t.c));
				BOOST_TEST(glm::length(hit->normal) == 1.f, boost::test_tools::tolerance(1e-3f));
			}
		}
	}
	BOOST_TEST(hits > 10);
}

BOOST_AUTO_TEST_CASE(TheNearestWalk_FindsTheNearestBox_OfABruteForce_AndVisitsLessThanAll)
{
	using tree = ds::bvh<int>;
	generator gen(51);
	std::vector<tree::item> items;
	for (int i = 0; i < 400; ++i) {
		const glm::vec3 centre = gen.point(50.f);
		const glm::vec3 half{ gen.real(0.1f, 2.f), gen.real(0.1f, 2.f), gen.real(0.1f, 2.f) };
		items.push_back({ ds::aabb3{ centre - half, centre + half }, i });
	}
	const tree walk{ items };
	for (int run = 0; run < 40; ++run) {
		const glm::vec3 p = gen.point(60.f);
		float brute = std::numeric_limits<float>::infinity();
		for (const tree::item& each : items) brute = std::min(brute, ds::distance_to_box(p, each.box));
		float found = std::numeric_limits<float>::infinity();
		int visited = 0;
		walk.query_nearest(p, std::numeric_limits<float>::infinity(), [&](std::uint32_t, int, float distance) {
			++visited;
			found = std::min(found, distance);
			return ds::bvh_nearest_action{ found, false };
		});
		BOOST_TEST_CONTEXT("run " << run) {
			BOOST_TEST(found == brute, boost::test_tools::tolerance(1e-5f));
			BOOST_TEST(visited < 400);
		}
	}
}

BOOST_AUTO_TEST_CASE(TheNearestWalk_StaysWithinTheDistanceItIsGiven_AndStopsWhenAsked)
{
	using tree = ds::bvh<int>;
	std::vector<tree::item> items;
	for (int i = 0; i < 50; ++i) {
		const glm::vec3 centre{ static_cast<float>(i) * 3.f, 0.f, 0.f };
		items.push_back({ ds::aabb3{ centre - glm::vec3{ 1.f }, centre + glm::vec3{ 1.f } }, i });
	}
	items.push_back({ ds::aabb3{}, 99 });
	const tree walk{ items };
	std::vector<int> seen;
	walk.query_nearest({ 0.f, 0.f, 0.f }, 5.f, [&](std::uint32_t, int value, float) {
		seen.push_back(value);
		return ds::bvh_nearest_action{ 5.f, false };
	});
	std::sort(seen.begin(), seen.end());
	BOOST_TEST((seen == std::vector<int>{ 0, 1, 2 }));

	int count = 0;
	walk.query_nearest({ 0.f, 0.f, 0.f }, std::numeric_limits<float>::infinity(), [&](std::uint32_t, int, float) {
		++count;
		return ds::bvh_nearest_action{ std::numeric_limits<float>::infinity(), true };
	});
	BOOST_TEST(count == 1);

	const tree none;
	none.query_nearest({ 0.f, 0.f, 0.f }, 10.f, [&](std::uint32_t, int, float) {
		BOOST_FAIL("an empty tree has nothing to visit");
		return ds::bvh_nearest_action{};
	});
	BOOST_TEST(ds::distance_to_box({ 0.f, 0.f, 0.f }, ds::aabb3{}) == std::numeric_limits<float>::infinity());
}


BOOST_AUTO_TEST_CASE(TheNearestPointOfANearlyCollinearTriangle_IsOnItsLine)
{
	// A case a fuzz found: the three regions of the plain formula were decided by rounding and the point left the triangle.
	const glm::vec3 a{ 3.2646768f, 0.13918376f, 0.23231745f };
	const glm::vec3 b{ 2.5168927f, -0.6002254f, -0.38513815f };
	const glm::vec3 c{ 0.24641013f, -2.8452787f, -2.2599068f };
	const glm::vec3 p{ 0.7232902f, -1.1988856f, -0.9770048f };
	const glm::vec3 nearest = ds::closest_point_on_triangle(p, a, b, c);
	BOOST_TEST(nearest.x == 1.436f, boost::test_tools::tolerance(1e-2f));
	BOOST_TEST(nearest.y == -1.669f, boost::test_tools::tolerance(1e-2f));
	BOOST_TEST(nearest.z == -1.278f, boost::test_tools::tolerance(1e-2f));

	generator gen(61);
	for (int run = 0; run < 2000; ++run) {
		const glm::vec3 origin = gen.point(3.f);
		const glm::vec3 along = gen.unit();
		const glm::vec3 v0 = origin + along * gen.real(-3.f, 3.f);
		const glm::vec3 v1 = origin + along * gen.real(-3.f, 3.f);
		const glm::vec3 v2 = origin + along * gen.real(-3.f, 3.f);
		const glm::vec3 q = gen.point(5.f);
		// On a line the answer is the nearest point of the segment between the two ends that are farthest apart.
		glm::vec3 lo = v0;
		glm::vec3 hi = v1;
		if (glm::distance(v0, v2) > glm::distance(lo, hi)) hi = v2;
		if (glm::distance(v1, v2) > glm::distance(lo, hi)) {
			lo = v1;
			hi = v2;
		}
		const float span = glm::dot(hi - lo, hi - lo);
		const glm::vec3 truth = span > 0.f ? lo + (hi - lo) * std::clamp(glm::dot(q - lo, hi - lo) / span, 0.f, 1.f) : lo;
		const glm::vec3 got = ds::closest_point_on_triangle(q, v0, v1, v2);
		BOOST_TEST_CONTEXT("run " << run) {
			BOOST_TEST(std::abs(glm::distance(q, got) - glm::distance(q, truth)) <= 1e-2f);
		}
	}
}

BOOST_AUTO_TEST_CASE(ANanOrInfinity_IsNeverAnOverlapOrAHit)
{
	const float nan = std::nanf("");
	const float inf = std::numeric_limits<float>::infinity();
	const triangle t{ { -1.f, 0.f, -1.f }, { 1.f, 0.f, -1.f }, { 0.f, 0.f, 1.f } };
	const triangle bad{ { nan, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	const ds::obb3 box{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f }, glm::mat3{ 1.f } };
	const ds::obb3 nan_box{ { nan, 0.f, 0.f }, { 1.f, 1.f, 1.f }, glm::mat3{ 1.f } };
	const ds::obb3 inf_box{ { 0.f, 0.f, 0.f }, { inf, 1.f, 1.f }, glm::mat3{ 1.f } };
	BOOST_TEST(!ds::obb_triangle(box, bad.a, bad.b, bad.c));
	BOOST_TEST(!ds::obb_triangle(nan_box, t.a, t.b, t.c));
	BOOST_TEST(!ds::obb_triangle(inf_box, t.a, t.b, t.c));
	BOOST_TEST(!ds::sphere_triangle({ { nan, 0.f, 0.f }, 1.f }, t.a, t.b, t.c));
	BOOST_TEST(!ds::sphere_triangle({ { 0.f, 0.f, 0.f }, 1.f }, bad.a, bad.b, bad.c));
	BOOST_TEST(!ds::capsule_triangle({ { nan, 0.f, 0.f }, { 0.f, 1.f, 0.f }, 1.f }, t.a, t.b, t.c));
	BOOST_TEST(!ds::capsule_triangle({ { 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, 1.f }, bad.a, bad.b, bad.c));
	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { nan, -1.f, 0.f }, t.a, t.b, t.c).has_value());
	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, nan }, { 0.f, -1.f, 0.f }, t.a, t.b, t.c).has_value());
	BOOST_TEST(!ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, -1.f, 0.f }, bad.a, bad.b, bad.c).has_value());
}

BOOST_AUTO_TEST_CASE(ADegenerateTriangle_AgainstABoxACapsuleAndASweep)
{
	const glm::vec3 point{ 0.f, 0.f, 0.f };
	const ds::obb3 box{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f }, glm::mat3{ 1.f } };
	BOOST_TEST(ds::obb_triangle(box, point, point, point));
	BOOST_TEST(!ds::obb_triangle(box, { 5.f, 0.f, 0.f }, { 5.f, 0.f, 0.f }, { 5.f, 0.f, 0.f }));
	BOOST_TEST(ds::obb_triangle(box, { -3.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 3.f, 0.f, 0.f }));
	BOOST_TEST(ds::capsule_triangle({ { 1.f, 0.f, 0.f }, { 1.f, 2.f, 0.f }, 1.f }, point, point, point));
	BOOST_TEST(!ds::capsule_triangle({ { 3.f, 0.f, 0.f }, { 3.f, 2.f, 0.f }, 1.f }, point, point, point));

	// A triangle with no area is its segments: a sphere falls on the line and stops a radius short of it.
	const auto line = ds::sphere_sweep_triangle({ { 1.f, 3.f, 0.f }, 1.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { 4.f, 0.f, 0.f });
	BOOST_REQUIRE(line.has_value());
	BOOST_TEST(line->t == 2.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(line->normal.y == 1.f, boost::test_tools::tolerance(1e-4f));
	const auto point_hit = ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 1.f }, { 0.f, -1.f, 0.f }, point, point, point);
	BOOST_REQUIRE(point_hit.has_value());
	BOOST_TEST(point_hit->t == 4.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(ASweepInThePlaneOfTheTriangle_HitsItsEdge_AndOneAlongTheFaceMisses)
{
	const triangle t{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const auto in_plane = ds::sphere_sweep_triangle({ { 0.f, 0.f, -20.f }, 1.f }, { 0.f, 0.f, 1.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(in_plane.has_value());
	BOOST_TEST(in_plane->t == 9.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(in_plane->normal.z == -1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(!ds::sphere_sweep_triangle({ { -20.f, 3.f, 0.f }, 1.f }, { 1.f, 0.f, 0.f }, t.a, t.b, t.c).has_value());
}

BOOST_AUTO_TEST_CASE(ASphereThatTouchesAtTheStart_StartsInside_WhetherItMovesInOrAway)
{
	const triangle t{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	for (const float way : { -1.f, 1.f }) {
		const auto hit = ds::sphere_sweep_triangle({ { 0.f, 2.f, 0.f }, 2.f }, { 0.f, way, 0.f }, t.a, t.b, t.c);
		BOOST_REQUIRE(hit.has_value());
		BOOST_TEST(hit->started_inside);
		BOOST_TEST(hit->t == 0.f);
		BOOST_TEST(hit->normal.y == 1.f, boost::test_tools::tolerance(1e-4f));
	}
}

BOOST_AUTO_TEST_CASE(ASphereWithNoRadius_IsAPoint_AndABoxWithNoVolume_IsAPlaneOrAPoint)
{
	const triangle t{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const auto hit = ds::sphere_sweep_triangle({ { 0.f, 5.f, 0.f }, 0.f }, { 0.f, -1.f, 0.f }, t.a, t.b, t.c);
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->t == 5.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(ds::sphere_triangle({ { 0.f, 0.f, 0.f }, 0.f }, t.a, t.b, t.c));
	BOOST_TEST(!ds::sphere_triangle({ { 0.f, 0.1f, 0.f }, 0.f }, t.a, t.b, t.c));

	const ds::obb3 flat{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, glm::mat3{ 1.f } };
	BOOST_TEST(ds::obb_triangle(flat, { -1.f, -1.f, -1.f }, { 1.f, 1.f, -1.f }, { 0.f, 0.f, 1.f }));
	BOOST_TEST(!ds::obb_triangle(flat, { -1.f, -1.f, 1.f }, { 1.f, 1.f, 1.f }, { 0.f, 0.f, 2.f }));
	const ds::obb3 nothing{ { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, glm::mat3{ 1.f } };
	BOOST_TEST(ds::obb_triangle(nothing, { -1.f, -1.f, 0.f }, { 1.f, -1.f, 0.f }, { 0.f, 1.f, 0.f }));
	BOOST_TEST(!ds::obb_triangle(nothing, { -1.f, -1.f, 0.5f }, { 1.f, -1.f, 0.5f }, { 0.f, 1.f, 0.5f }));
}


BOOST_AUTO_TEST_CASE(ACapsuleFalling_StopsAtItsEnd_OrItsSide_WithTheFloorsNormal)
{
	const triangle floor{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const auto upright = ds::capsule_sweep_triangle({ { 0.f, 1.f, 0.f }, { 0.f, 2.f, 0.f }, 0.5f }, { 0.f, -1.f, 0.f }, floor.a, floor.b, floor.c);
	BOOST_REQUIRE(upright.has_value());
	BOOST_TEST(upright->t == 0.5f, boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(upright->point.y == 0.f, boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(upright->normal.y == 1.f, boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(!upright->started_inside);

	const auto lying = ds::capsule_sweep_triangle({ { -1.f, 2.f, 0.f }, { 1.f, 2.f, 0.f }, 0.5f }, { 0.f, -1.f, 0.f }, floor.a, floor.b, floor.c);
	BOOST_REQUIRE(lying.has_value());
	BOOST_TEST(lying->t == 1.5f, boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(lying->normal.y == 1.f, boost::test_tools::tolerance(1e-3f));
}

BOOST_AUTO_TEST_CASE(ACapsuleThatStartsTouching_Misses_IsTooShort_OrHasNoDirection_IsTold)
{
	const triangle floor{ { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f }, { 0.f, 0.f, 10.f } };
	const ds::capsule3 deep{ { 0.f, 0.3f, 0.f }, { 0.f, 1.3f, 0.f }, 0.5f };
	const auto inside = ds::capsule_sweep_triangle(deep, { 1.f, 0.f, 0.f }, floor.a, floor.b, floor.c);
	BOOST_REQUIRE(inside.has_value());
	BOOST_TEST(inside->started_inside);
	BOOST_TEST(inside->t == 0.f);
	BOOST_TEST(inside->normal.y == 1.f, boost::test_tools::tolerance(1e-3f));

	const ds::capsule3 high{ { 0.f, 3.f, 0.f }, { 0.f, 4.f, 0.f }, 0.5f };
	BOOST_TEST(!ds::capsule_sweep_triangle(high, { 0.f, 1.f, 0.f }, floor.a, floor.b, floor.c).has_value());
	BOOST_TEST(!ds::capsule_sweep_triangle(high, { 1.f, 0.f, 0.f }, floor.a, floor.b, floor.c).has_value());
	BOOST_TEST(!ds::capsule_sweep_triangle(high, { 0.f, -1.f, 0.f }, floor.a, floor.b, floor.c, 2.f).has_value());
	BOOST_TEST(ds::capsule_sweep_triangle(high, { 0.f, -1.f, 0.f }, floor.a, floor.b, floor.c, 2.6f).has_value());
	BOOST_TEST(!ds::capsule_sweep_triangle(high, { 0.f, 0.f, 0.f }, floor.a, floor.b, floor.c).has_value());
	BOOST_TEST(!ds::capsule_sweep_triangle(high, { std::nanf(""), -1.f, 0.f }, floor.a, floor.b, floor.c).has_value());
	BOOST_TEST(!ds::capsule_sweep_triangle(high, { 0.f, -1.f, 0.f }, { std::nanf(""), 0.f, 0.f }, floor.b, floor.c).has_value());
}

BOOST_AUTO_TEST_CASE(ACapsuleSweep_NeverGoesPastTheTouch_AndStopsWithinItsToleranceOfIt)
{
	generator gen(71);
	constexpr float k_reach = 8.f;
	constexpr float k_step = 0.01f;
	int hits = 0;
	int missed = 0;
	for (int run = 0; run < 300; ++run) {
		const triangle t = gen.make_triangle();
		const glm::vec3 a = gen.point(3.f);
		const ds::capsule3 capsule{ a, a + gen.unit() * gen.real(0.f, 2.f), gen.real(0.2f, 1.f) };
		const glm::vec3 direction = gen.unit();
		const auto hit = ds::capsule_sweep_triangle(capsule, direction, t.a, t.b, t.c, k_reach);
		std::optional<float> marched;
		for (float at = 0.f; at <= k_reach; at += k_step) {
			const glm::vec3 shift = direction * at;
			if (ds::capsule_triangle({ capsule.a + shift, capsule.b + shift, capsule.radius }, t.a, t.b, t.c)) {
				marched = at;
				break;
			}
		}
		BOOST_TEST_CONTEXT("run " << run) {
			if (marched) {
				if (hit) {
					BOOST_TEST(hit->t <= *marched + 1e-3f);
				} else {
					++missed;
				}
			}
			if (hit && !hit->started_inside) {
				++hits;
				const glm::vec3 shift = direction * hit->t;
				const float gap = std::sqrt(ds::segment_triangle_distance_sq(capsule.a + shift, capsule.b + shift, t.a, t.b, t.c)) - capsule.radius;
				BOOST_TEST(gap <= 2e-4f);
				BOOST_TEST(gap >= -1e-3f);
				BOOST_TEST(glm::length(hit->normal) == 1.f, boost::test_tools::tolerance(1e-3f));
			}
		}
	}
	BOOST_TEST(hits > 10);
	// A motion that grazes closes the gap so slowly that the steps run out first.
	BOOST_TEST(missed <= 6);
}

BOOST_AUTO_TEST_CASE(ACapsuleThatIsAPoint_SweepsAsASphereDoes)
{
	generator gen(81);
	int compared = 0;
	for (int run = 0; run < 300; ++run) {
		const triangle t = gen.make_triangle();
		const glm::vec3 centre = gen.point(3.f);
		const float radius = gen.real(0.2f, 1.f);
		const glm::vec3 direction = gen.unit();
		const auto sphere = ds::sphere_sweep_triangle({ centre, radius }, direction, t.a, t.b, t.c, 8.f);
		const auto capsule = ds::capsule_sweep_triangle({ centre, centre, radius }, direction, t.a, t.b, t.c, 8.f);
		BOOST_TEST_CONTEXT("run " << run) {
			if (capsule) {
				BOOST_REQUIRE(sphere.has_value());
				BOOST_TEST(capsule->t <= sphere->t + 1e-3f);
				BOOST_TEST(capsule->started_inside == sphere->started_inside);
			}
			if (capsule && sphere && !sphere->started_inside) {
				++compared;
				const glm::vec3 at = centre + direction * capsule->t;
				const float gap = glm::distance(at, ds::closest_point_on_triangle(at, t.a, t.b, t.c)) - radius;
				BOOST_TEST(gap <= 2e-4f);
				BOOST_TEST(gap >= -1e-3f);
			}
		}
	}
	BOOST_TEST(compared > 10);
}


BOOST_AUTO_TEST_CASE(ACapsuleCastAtAGentleAngleOrALongWay_StillStopsWhereItTouches)
{
	// A floor wide enough for the longest of these, a capsule standing on its end above it, moving down at a shallow angle: the lowest
	// point of its lower cap reaches the radius from the floor at t = (height - radius) / sin(angle).
	const triangle floor{ { -2e5f, 0.f, -2e5f }, { 2e5f, 0.f, -2e5f }, { 0.f, 0.f, 2e5f } };
	for (const float degrees : { 0.5f, 1.f, 3.f, 20.f }) {
		for (const float height : { 10.f, 100.f, 300.f }) {
			const float angle = glm::radians(degrees);
			const glm::vec3 direction{ std::cos(angle), -std::sin(angle), 0.f };
			const auto hit = ds::capsule_sweep_triangle({ { 0.f, height, 0.f }, { 0.f, height + 1.f, 0.f }, 0.5f }, direction, floor.a, floor.b, floor.c);
			BOOST_TEST_CONTEXT(degrees << " degrees from " << height) {
				BOOST_REQUIRE(hit.has_value());
				BOOST_TEST(hit->t == (height - 0.5f) / std::sin(angle), boost::test_tools::tolerance(2e-3f));
				BOOST_TEST(hit->normal.y == 1.f, boost::test_tools::tolerance(1e-3f));
			}
		}
	}
	// Thousands of units: where a stepped search ran out of steps or of precision.
	for (const float height : { 1500.f, 3000.f }) {
		const glm::vec3 direction{ std::cos(glm::radians(20.f)), -std::sin(glm::radians(20.f)), 0.f };
		const auto hit = ds::capsule_sweep_triangle({ { 0.f, height, 0.f }, { 0.5f, height + 1.f, 0.f }, 0.5f }, direction, floor.a, floor.b, floor.c);
		BOOST_REQUIRE(hit.has_value());
		BOOST_TEST(hit->t == (height - 0.5f) / std::sin(glm::radians(20.f)), boost::test_tools::tolerance(2e-3f));
	}
}

BOOST_AUTO_TEST_CASE(ACapsuleSide_MeetsACornerAndAnEdgeOfATriangle_BeforeItsEnds)
{
	// A capsule along Z at the origin, a triangle coming at it from the side: its corner touches the capsule's side at x = -radius.
	const ds::capsule3 capsule{ { 0.f, 0.f, -2.f }, { 0.f, 0.f, 2.f }, 0.5f };
	const auto corner = ds::capsule_sweep_triangle(capsule, { 1.f, 0.f, 0.f }, { 3.f, 0.f, 0.f }, { 4.f, 1.f, 0.f }, { 4.f, -1.f, 0.f });
	// The capsule moves +x and the triangle lies at x >= 3: the capsule's side meets the corner (3, 0, 0) where x + 0.5 = 3.
	BOOST_REQUIRE(corner.has_value());
	BOOST_TEST(corner->t == 2.5f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(corner->point.x == 3.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(corner->normal.x == -1.f, boost::test_tools::tolerance(1e-3f));

	// An edge of the triangle running along Y, standing across the capsule's axis: it touches the side too.
	const auto edge = ds::capsule_sweep_triangle(capsule, { 1.f, 0.f, 0.f }, { 3.f, -1.f, -1.f }, { 3.f, 1.f, -1.f }, { 3.f, 0.f, 1.f });
	BOOST_REQUIRE(edge.has_value());
	BOOST_TEST(edge->t == 2.5f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(edge->normal.x == -1.f, boost::test_tools::tolerance(1e-3f));

	// The same triangle beyond the capsule's end (z in [3, 5]) is missed: neither side nor cap reaches it.
	BOOST_TEST(!ds::capsule_sweep_triangle(capsule, { 1.f, 0.f, 0.f }, { 3.f, -1.f, 3.f }, { 3.f, 1.f, 3.f }, { 3.f, 0.f, 5.f }).has_value());
}

BOOST_AUTO_TEST_CASE(TheSurfaceOfABox_IsItsTwelveTrianglesThroughTheMatrix)
{
	const ds::aabb3 box{ { -1.f, -2.f, -3.f }, { 1.f, 2.f, 3.f } };
	const auto faces = ds::box_surface(box, glm::mat4{ 1.f });
	float area = 0.f;
	for (const ds::triangle3& face : faces) area += 0.5f * glm::length(glm::cross(face.b - face.a, face.c - face.a));
	BOOST_TEST(area == 2.f * (2.f * 4.f + 2.f * 6.f + 4.f * 6.f), boost::test_tools::tolerance(1e-4f));
	const auto hit = ds::sphere_sweep_triangle({ { 0.f, 0.f, 9.f }, 1.f }, { 0.f, 0.f, -1.f }, faces[10].a, faces[10].b, faces[10].c);
	// faces[10] and [11] are the +z face: a sphere falling on it stops a radius above z = 3.
	BOOST_TEST((hit.has_value() || ds::sphere_sweep_triangle({ { 0.f, 0.f, 9.f }, 1.f }, { 0.f, 0.f, -1.f }, faces[11].a, faces[11].b, faces[11].c).has_value()));
	const glm::mat4 moved = glm::translate(glm::mat4{ 1.f }, glm::vec3{ 10.f, 0.f, 0.f });
	BOOST_TEST(ds::box_surface(box, moved)[0].a.x >= 9.f - 1e-4f);
}

BOOST_AUTO_TEST_SUITE_END()
