#include <boost/test/unit_test.hpp>
#include <snk/common/ds/ds_aabb3.hpp>
#include <snk/common/ds/ds_intersect.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>

namespace {
constexpr float k_eps = 1e-4f;

bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) <= k_eps; }
}

BOOST_AUTO_TEST_SUITE(DsAabb3Tests)

BOOST_AUTO_TEST_CASE(ADefaultBox_IsEmpty_AndChangesNothingItIsExpandedBy)
{
	ds::aabb3 box;
	BOOST_TEST(box.empty());
	ds::expand(box, ds::aabb3{});
	BOOST_TEST(box.empty());
	ds::expand(box, glm::vec3{ 1.f, 2.f, 3.f });
	BOOST_TEST(!box.empty());
	BOOST_TEST((box.min == glm::vec3{ 1.f, 2.f, 3.f }));
	BOOST_TEST((box.max == glm::vec3{ 1.f, 2.f, 3.f }));
	ds::expand(box, ds::aabb3{});
	BOOST_TEST((box.max == glm::vec3{ 1.f, 2.f, 3.f }));
}

BOOST_AUTO_TEST_CASE(ExpandByABox_TakesTheUnion)
{
	ds::aabb3 a{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f } };
	ds::expand(a, ds::aabb3{ { -1.f, 0.5f, 0.f }, { 0.5f, 3.f, 0.5f } });
	BOOST_TEST((a.min == glm::vec3{ -1.f, 0.f, 0.f }));
	BOOST_TEST((a.max == glm::vec3{ 1.f, 3.f, 1.f }));
}

BOOST_AUTO_TEST_CASE(IntersectsAndContains_TouchingCounts_EmptyNeverIntersects)
{
	const ds::aabb3 a{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f } };
	BOOST_TEST(ds::intersects(a, ds::aabb3{ { 1.f, 1.f, 1.f }, { 2.f, 2.f, 2.f } }));
	BOOST_TEST(!ds::intersects(a, ds::aabb3{ { 1.1f, 0.f, 0.f }, { 2.f, 1.f, 1.f } }));
	BOOST_TEST(!ds::intersects(a, ds::aabb3{}));
	BOOST_TEST(!ds::intersects(ds::aabb3{}, ds::aabb3{}));
	BOOST_TEST(ds::contains(a, glm::vec3{ 0.5f }));
	BOOST_TEST(ds::contains(a, glm::vec3{ 1.f }));
	BOOST_TEST(!ds::contains(a, glm::vec3{ 1.01f, 0.5f, 0.5f }));
}

BOOST_AUTO_TEST_CASE(SurfaceArea_OfABoxAndOfNothing)
{
	BOOST_TEST(ds::surface_area(ds::aabb3{ { 0.f, 0.f, 0.f }, { 1.f, 2.f, 3.f } }) == 2.f * (2.f + 6.f + 3.f));
	BOOST_TEST(ds::surface_area(ds::aabb3{}) == 0.f);
	BOOST_TEST(ds::surface_area(ds::aabb3{ { 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f } }) == 0.f);
}

BOOST_AUTO_TEST_CASE(Transformed_IsTheBoxOfTheTransformedCorners)
{
	const ds::aabb3 box{ { -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f } };
	const glm::mat4 turn = glm::rotate(glm::mat4{ 1.f }, glm::radians(45.f), glm::vec3{ 0.f, 0.f, 1.f });
	const ds::aabb3 turned = ds::transformed(box, turn);
	BOOST_TEST(turned.max.x == std::sqrt(2.f), boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(turned.max.z == 1.f, boost::test_tools::tolerance(1e-4f));
	const ds::aabb3 moved = ds::transformed(box, glm::translate(glm::mat4{ 1.f }, glm::vec3{ 5.f, 0.f, 0.f }));
	BOOST_TEST((moved.min == glm::vec3{ 4.f, -1.f, -1.f }));
	BOOST_TEST(ds::transformed(ds::aabb3{}, turn).empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(DsIntersectTests)

BOOST_AUTO_TEST_CASE(RayAabb_HitsInFront_ReturnsTheEntryDistance)
{
	const ds::aabb3 box{ { -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f } };
	const ds::ray3 ray{ { 0.f, 0.f, 5.f }, { 0.f, 0.f, -1.f } };
	const auto entry = ds::ray_aabb(ray, box);
	BOOST_REQUIRE(entry.has_value());
	BOOST_TEST(*entry == 4.f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(RayAabb_OriginInside_EntersAtZero)
{
	const ds::aabb3 box{ { -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f } };
	const auto entry = ds::ray_aabb(ds::ray3{ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f } }, box);
	BOOST_REQUIRE(entry.has_value());
	BOOST_TEST(*entry == 0.f);
}

BOOST_AUTO_TEST_CASE(RayAabb_MissesBesideBehindAndBeyondTheMaxDistance)
{
	const ds::aabb3 box{ { -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f } };
	BOOST_TEST(!ds::ray_aabb(ds::ray3{ { 2.f, 0.f, 5.f }, { 0.f, 0.f, -1.f } }, box).has_value());
	BOOST_TEST(!ds::ray_aabb(ds::ray3{ { 0.f, 0.f, 5.f }, { 0.f, 0.f, 1.f } }, box).has_value());
	BOOST_TEST(!ds::ray_aabb(ds::ray3{ { 0.f, 0.f, 5.f }, { 0.f, 0.f, -1.f } }, box, 3.f).has_value());
	BOOST_TEST(ds::ray_aabb(ds::ray3{ { 0.f, 0.f, 5.f }, { 0.f, 0.f, -1.f } }, box, 4.f).has_value());
	BOOST_TEST(!ds::ray_aabb(ds::ray3{ { 0.f, 0.f, 5.f }, { 0.f, 0.f, -1.f } }, ds::aabb3{}).has_value());
}

BOOST_AUTO_TEST_CASE(RayAabb_AnAxisAlignedRayOnAFacePlane_CountsAsTouching_OneOffThePlaneMisses)
{
	const ds::aabb3 box{ { -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f } };
	// y == 1 is the top face's plane and the direction has no y: the slab test must not turn that into NaN and lose the hit.
	BOOST_TEST(ds::ray_aabb(ds::ray3{ { -5.f, 1.f, 0.f }, { 1.f, 0.f, 0.f } }, box).has_value());
	BOOST_TEST(!ds::ray_aabb(ds::ray3{ { -5.f, 1.0001f, 0.f }, { 1.f, 0.f, 0.f } }, box).has_value());
}

BOOST_AUTO_TEST_CASE(RayAabb_ADiagonalRayThroughACorner)
{
	const ds::aabb3 box{ { 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f } };
	BOOST_TEST(ds::ray_aabb(ds::ray3{ { -1.f, -1.f, -1.f }, glm::normalize(glm::vec3{ 1.f, 1.f, 1.f }) }, box).has_value());
}

BOOST_AUTO_TEST_CASE(RayTriangle_Hit_GivesDistanceAndBarycentrics)
{
	const glm::vec3 a{ 0.f, 0.f, 0.f };
	const glm::vec3 b{ 4.f, 0.f, 0.f };
	const glm::vec3 c{ 0.f, 4.f, 0.f };
	const glm::vec3 origin{ 1.f, 2.f, 3.f };
	const auto hit = ds::ray_triangle(origin, { 0.f, 0.f, -1.f }, a, b, c);
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->t == 3.f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(hit->u == 0.25f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(hit->v == 0.5f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(near((1.f - hit->u - hit->v) * a + hit->u * b + hit->v * c, glm::vec3{ 1.f, 2.f, 0.f }));
	BOOST_TEST(!hit->back_face);
}

BOOST_AUTO_TEST_CASE(RayTriangle_FromBehind_IsABackFace_AndCullingDropsIt)
{
	const glm::vec3 a{ 0.f, 0.f, 0.f };
	const glm::vec3 b{ 4.f, 0.f, 0.f };
	const glm::vec3 c{ 0.f, 4.f, 0.f };
	const auto behind = ds::ray_triangle({ 1.f, 1.f, -3.f }, { 0.f, 0.f, 1.f }, a, b, c);
	BOOST_REQUIRE(behind.has_value());
	BOOST_TEST(behind->back_face);
	BOOST_TEST(!ds::ray_triangle({ 1.f, 1.f, -3.f }, { 0.f, 0.f, 1.f }, a, b, c, true).has_value());
	BOOST_TEST(ds::ray_triangle({ 1.f, 1.f, 3.f }, { 0.f, 0.f, -1.f }, a, b, c, true).has_value());
}

BOOST_AUTO_TEST_CASE(RayTriangle_MissesOutsideParallelBehindAndBeyondTheMax)
{
	const glm::vec3 a{ 0.f, 0.f, 0.f };
	const glm::vec3 b{ 4.f, 0.f, 0.f };
	const glm::vec3 c{ 0.f, 4.f, 0.f };
	BOOST_TEST(!ds::ray_triangle({ 3.f, 3.f, 1.f }, { 0.f, 0.f, -1.f }, a, b, c).has_value());
	BOOST_TEST(!ds::ray_triangle({ 1.f, 1.f, 1.f }, { 1.f, 0.f, 0.f }, a, b, c).has_value());
	BOOST_TEST(!ds::ray_triangle({ 1.f, 1.f, 1.f }, { 0.f, 0.f, 1.f }, a, b, c).has_value());
	BOOST_TEST(!ds::ray_triangle({ 1.f, 1.f, 3.f }, { 0.f, 0.f, -1.f }, a, b, c, false, 2.f).has_value());
	BOOST_TEST(!ds::ray_triangle({ 1.f, 1.f, 3.f }, { 0.f, 0.f, -1.f }, a, a, a).has_value());
}

BOOST_AUTO_TEST_CASE(RayTriangle_OnAnEdgeAndAVertex_Hits)
{
	const glm::vec3 a{ 0.f, 0.f, 0.f };
	const glm::vec3 b{ 4.f, 0.f, 0.f };
	const glm::vec3 c{ 0.f, 4.f, 0.f };
	BOOST_TEST(ds::ray_triangle({ 2.f, 0.f, 1.f }, { 0.f, 0.f, -1.f }, a, b, c).has_value());
	BOOST_TEST(ds::ray_triangle({ 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f }, a, b, c).has_value());
	BOOST_TEST(ds::ray_triangle({ 2.f, 2.f, 1.f }, { 0.f, 0.f, -1.f }, a, b, c).has_value());
}

BOOST_AUTO_TEST_CASE(Frustum_ClassifiesBoxesAgainstAPerspectiveView)
{
	const glm::mat4 view = glm::lookAt(glm::vec3{ 0.f, 0.f, 0.f }, glm::vec3{ 0.f, 0.f, -1.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(90.f), 1.f, 1.f, 100.f);
	const ds::frustum frustum = ds::frustum_from(projection * view);
	// In front, inside the 90 degree cone.
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{ { -1.f, -1.f, -11.f }, { 1.f, 1.f, -9.f } }) == ds::frustum_test::inside));
	// Behind the camera, beside it, and past the far plane.
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{ { -1.f, -1.f, 5.f }, { 1.f, 1.f, 7.f } }) == ds::frustum_test::outside));
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{ { 50.f, -1.f, -10.f }, { 52.f, 1.f, -8.f } }) == ds::frustum_test::outside));
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{ { -1.f, -1.f, -200.f }, { 1.f, 1.f, -150.f } }) == ds::frustum_test::outside));
	// Crossing the right side, and crossing the near plane.
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{ { 5.f, -1.f, -10.f }, { 15.f, 1.f, -8.f } }) == ds::frustum_test::intersects));
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{ { -0.1f, -0.1f, -2.f }, { 0.1f, 0.1f, 1.f } }) == ds::frustum_test::intersects));
	BOOST_TEST((ds::test_frustum(frustum, ds::aabb3{}) == ds::frustum_test::outside));
}

BOOST_AUTO_TEST_SUITE_END()
