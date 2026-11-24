#include <boost/test/unit_test.hpp>
#include "test_quad_geometry.hpp"
#include "test_scene_env.hpp"
#include <snk/common/ds/ds_intersect.hpp>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_spatial.h>
#include <glm/gtc/matrix_transform.hpp>
#include <entt/entt.hpp>
#include <cmath>
#include <limits>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// #1326: rays against the triangles of the loaded geometry a world holds.

namespace {

struct spatial_env : test_util::scene_env
{
	spatial_env() { descs->register_desc<rnd::geometry_desc>("geometry_desc"); }
	~spatial_env() { descs->unregister_desc("geometry_desc"); }

	// A unit square in the XY plane, two counter-clockwise triangles: the front faces +Z.
	void put_quad(std::string_view name, float half = 0.5f)
	{
		geometry = require_loaded<rnd::geometry_desc>(put(name, test_util::quad_geometry_json(half)));
		BOOST_REQUIRE(geometry.is_ready());
	}

	entt::entity add_quad(entt::registry& reg, const glm::mat4& world)
	{
		scn::geometry_holds::attach(reg, rs.get());
		const auto entity = reg.create();
		reg.emplace<scn::world_transform>(entity, scn::world_transform{ world });
		reg.emplace<scn::renderable>(entity);
		reg.emplace<scn::mesh_component>(entity, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 6 } });
		reg.emplace<scn::geometry_component>(entity, scn::geometry_component{ .geom_tag = geometry.get_tag() });
		BOOST_REQUIRE(scn::geometry_holds::of(reg).find(entity));
		return entity;
	}

	res::res_handle<rnd::geometry_desc> geometry;
};

glm::mat4 at(glm::vec3 position) { return glm::translate(glm::mat4{ 1.f }, position); }

scn::ray toward_minus_z(glm::vec3 origin) { return scn::ray{ origin, glm::vec3{ 0.f, 0.f, -1.f } }; }

}

BOOST_FIXTURE_TEST_SUITE(ScnSpatial, spatial_env)

BOOST_AUTO_TEST_CASE(ARay_HitsTheFrontOfAQuad_WithDistancePointNormalAndTriangle)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto hit = scn::raycast_closest(reg, toward_minus_z({ 0.1f, 0.1f, 2.f }));
	BOOST_REQUIRE(hit);
	BOOST_TEST((hit->entity == quad));
	BOOST_TEST(hit->distance == 5.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->point.z == -3.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->normal.z == 1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(!hit->back_face);
	BOOST_TEST(hit->triangle < 2u);
}

BOOST_AUTO_TEST_CASE(ARayBesideTheQuad_Misses)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, at({ 0.f, 0.f, -3.f }));
	BOOST_TEST(!scn::raycast_closest(reg, toward_minus_z({ 2.f, 0.f, 2.f })));
	BOOST_TEST(!scn::raycast_any(reg, toward_minus_z({ 2.f, 0.f, 2.f })));
}

BOOST_AUTO_TEST_CASE(ARayFromBehind_IsABackFaceHit_UnlessBackFacesAreCulled)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const scn::ray from_behind{ { 0.f, 0.f, -10.f }, { 0.f, 0.f, 1.f } };

	const auto hit = scn::raycast_closest(reg, from_behind);
	BOOST_REQUIRE(hit);
	BOOST_TEST(hit->back_face);
	BOOST_TEST(hit->normal.z == -1.f, boost::test_tools::tolerance(1e-4f));

	scn::ray_filter culling;
	culling.cull_back_faces = true;
	BOOST_TEST(!scn::raycast_closest(reg, from_behind, culling));
}

BOOST_AUTO_TEST_CASE(AMirroredScale_TurnsTheFrontIntoABack)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, glm::scale(at({ 0.f, 0.f, -3.f }), glm::vec3{ -1.f, 1.f, 1.f }));
	const auto hit = scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f }));
	BOOST_REQUIRE(hit);
	BOOST_TEST(hit->back_face);
	BOOST_TEST(hit->normal.z == 1.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(ScaleAndRotation_AreInTheDistanceAndTheNormal)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const glm::mat4 matrix = glm::rotate(glm::scale(at({ 0.f, 0.f, -4.f }), glm::vec3{ 4.f }), glm::radians(90.f), glm::vec3{ 0.f, 1.f, 0.f });
	add_quad(reg, matrix);
	// The quad now stands in the YZ plane, four units wide, its front toward +X.
	const scn::ray along_x{ { 5.f, 0.5f, -4.5f }, { -1.f, 0.f, 0.f } };
	const auto hit = scn::raycast_closest(reg, along_x);
	BOOST_REQUIRE(hit);
	BOOST_TEST(hit->distance == 5.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->normal.x == 1.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(TheNearerOfTwoQuadsWins_AndAllListsBothNearestFirst)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto far_quad = add_quad(reg, at({ 0.f, 0.f, -8.f }));
	const auto near_quad = add_quad(reg, at({ 0.f, 0.f, -2.f }));
	const auto ray = toward_minus_z({ 0.f, 0.f, 1.f });

	const auto closest = scn::raycast_closest(reg, ray);
	BOOST_REQUIRE(closest);
	BOOST_TEST((closest->entity == near_quad));

	std::vector<scn::ray_hit> all;
	scn::raycast_all(reg, ray, {}, all);
	BOOST_REQUIRE(all.size() == 2u);
	BOOST_TEST((all[0].entity == near_quad));
	BOOST_TEST((all[1].entity == far_quad));
	BOOST_TEST(all[0].distance < all[1].distance);
}

BOOST_AUTO_TEST_CASE(TheFilters_SkipMaxDistanceExcludedHiddenAndOtherLayers)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto ray = toward_minus_z({ 0.f, 0.f, 2.f });

	scn::ray_filter too_short;
	too_short.max_distance = 4.f;
	BOOST_TEST(!scn::raycast_closest(reg, ray, too_short));

	const std::vector<entt::entity> skipped{ quad };
	scn::ray_filter excluding;
	excluding.exclude = skipped;
	BOOST_TEST(!scn::raycast_closest(reg, ray, excluding));

	reg.remove<scn::renderable>(quad);
	BOOST_TEST(!scn::raycast_closest(reg, ray));
	scn::ray_filter with_hidden;
	with_hidden.include_hidden = true;
	BOOST_TEST(scn::raycast_closest(reg, ray, with_hidden).has_value());

	reg.emplace<scn::spatial_layers>(quad, scn::spatial_layers{ scn::spatial_mask{ scn::spatial_layer{ 3 } } });
	scn::ray_filter default_layer;
	default_layer.include_hidden = true;
	default_layer.mask = scn::spatial_mask{ scn::k_default_spatial_layer };
	BOOST_TEST(!scn::raycast_closest(reg, ray, default_layer));
	default_layer.mask = scn::spatial_mask{ scn::spatial_layer{ 3 } };
	BOOST_TEST(scn::raycast_closest(reg, ray, default_layer).has_value());
}

BOOST_AUTO_TEST_CASE(WithoutTriangles_TheBoxOfTheMeshAnswers)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, at({ 0.f, 0.f, -3.f }));
	scn::ray_filter boxes;
	boxes.triangles = false;
	const auto hit = scn::raycast_closest(reg, toward_minus_z({ 0.4f, -0.4f, 2.f }), boxes);
	BOOST_REQUIRE(hit);
	BOOST_TEST(hit->triangle == ~0u);
	BOOST_TEST(hit->distance == 5.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(AMovedEntity_IsHitWhereItIsAfterTheTransformSystemToldTheWorld)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto ray = toward_minus_z({ 0.f, 0.f, 2.f });
	BOOST_REQUIRE(scn::raycast_closest(reg, ray));

	reg.get<scn::world_transform>(quad).world = at({ 10.f, 0.f, -3.f });
	const std::vector<entt::entity> moved{ quad };
	scn::spatial_world::attach(reg).entities_moved(reg, moved);
	BOOST_TEST(!scn::raycast_closest(reg, ray));
	BOOST_TEST(scn::raycast_closest(reg, toward_minus_z({ 10.f, 0.f, 2.f })).has_value());
}

BOOST_AUTO_TEST_CASE(AMeshAddedOrDestroyedAfterTheFirstQuery_IsSeenByTheNext)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto first = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto ray = toward_minus_z({ 0.f, 0.f, 2.f });
	BOOST_REQUIRE(scn::raycast_closest(reg, ray));

	const auto nearer = add_quad(reg, at({ 0.f, 0.f, -1.f }));
	const auto hit = scn::raycast_closest(reg, ray);
	BOOST_REQUIRE(hit);
	BOOST_TEST((hit->entity == nearer));

	reg.destroy(nearer);
	reg.destroy(first);
	BOOST_TEST(!scn::raycast_closest(reg, ray));
}

BOOST_AUTO_TEST_CASE(AMeshWhoseGeometryIsNotLoaded_IsNotHit_AndNothingFails)
{
	entt::registry reg;
	scn::geometry_holds::attach(reg, rs.get());
	const auto entity = reg.create();
	reg.emplace<scn::world_transform>(entity);
	reg.emplace<scn::renderable>(entity);
	reg.emplace<scn::mesh_component>(entity, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 6 } });
	reg.emplace<scn::geometry_component>(entity, scn::geometry_component{ .geom_tag = tag_of("missing.geom.desc") });
	BOOST_TEST(!scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f })));
}

BOOST_AUTO_TEST_CASE(ANonUniformScale_StretchesTheHitAndKeepsTheNormalPerpendicular)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	// 4 wide, 1 tall, tilted 45 degrees about X: the normal is (0, -sin 45, cos 45) however the scale stretches the quad.
	glm::mat4 matrix = glm::rotate(at({ 0.f, 0.f, -3.f }), glm::radians(45.f), glm::vec3{ 1.f, 0.f, 0.f });
	matrix = glm::scale(matrix, glm::vec3{ 4.f, 1.f, 1.f });
	add_quad(reg, matrix);
	const auto hit = scn::raycast_closest(reg, toward_minus_z({ 1.8f, 0.f, 2.f }));
	BOOST_REQUIRE(hit);
	BOOST_TEST(hit->normal.y == -std::sin(glm::radians(45.f)), boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(hit->normal.z == std::cos(glm::radians(45.f)), boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(glm::length(hit->normal) == 1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(!scn::raycast_closest(reg, toward_minus_z({ 2.5f, 0.f, 2.f })));
}

BOOST_AUTO_TEST_CASE(AnEntityReplacedAtTheSameMeshCount_IsSeenAsTheNewOne)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto old_one = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	BOOST_REQUIRE(scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f })));

	reg.destroy(old_one);
	const auto replacement = add_quad(reg, at({ 5.f, 0.f, -3.f }));
	BOOST_TEST(!scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f })));
	const auto hit = scn::raycast_closest(reg, toward_minus_z({ 5.f, 0.f, 2.f }));
	BOOST_REQUIRE(hit);
	BOOST_TEST((hit->entity == replacement));
}

BOOST_AUTO_TEST_CASE(AMeshComponentGivenAnotherRange_IsHitByTheNewRange)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	BOOST_REQUIRE(scn::raycast_closest(reg, toward_minus_z({ -0.3f, 0.4f, 2.f })));
	// Only the first triangle (indices 0..3, the lower right half): the point (-0.3, 0.4) lies in the second one.
	reg.replace<scn::mesh_component>(quad, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 3 } });
	BOOST_TEST(!scn::raycast_closest(reg, toward_minus_z({ -0.3f, 0.4f, 2.f })));
	BOOST_TEST(scn::raycast_closest(reg, toward_minus_z({ 0.4f, -0.4f, 2.f })).has_value());
}

BOOST_AUTO_TEST_CASE(AMeshWhoseGeometryLoadsLater_IsHitOnceItIsLoaded)
{
	entt::registry reg;
	scn::geometry_holds::attach(reg, rs.get());
	const auto entity = reg.create();
	reg.emplace<scn::world_transform>(entity, scn::world_transform{ at({ 0.f, 0.f, -3.f }) });
	reg.emplace<scn::renderable>(entity);
	reg.emplace<scn::mesh_component>(entity, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 6 } });
	reg.emplace<scn::geometry_component>(entity, scn::geometry_component{ .geom_tag = tag_of("later.geom.desc") });
	const auto ray = toward_minus_z({ 0.f, 0.f, 2.f });
	BOOST_REQUIRE(!scn::raycast_closest(reg, ray));

	put("later.geom.desc", test_util::quad_geometry_json());
	rs->signal_changed(tag_of("later.geom.desc"));
	pump();
	BOOST_TEST(scn::raycast_closest(reg, ray).has_value());
}

BOOST_AUTO_TEST_CASE(AGeometryHeldAgainAsABiggerShape_IsHitWhereTheNewShapeIs)
{
	put_quad("small.geom.desc", 0.5f);
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto outside = toward_minus_z({ 1.5f, 0.f, 2.f });
	BOOST_REQUIRE(!scn::raycast_closest(reg, outside));

	put("big.geom.desc", test_util::quad_geometry_json(2.f));
	reg.replace<scn::geometry_component>(quad, scn::geometry_component{ .geom_tag = tag_of("big.geom.desc") });
	require_loaded<rnd::geometry_desc>(tag_of("big.geom.desc"));
	scn::geometry_holds::of(reg).hold(quad, tag_of("big.geom.desc"));
	BOOST_TEST(scn::raycast_closest(reg, outside).has_value());
}

BOOST_AUTO_TEST_CASE(AGeometryReloadedInPlace_IsHitWhereTheNewShapeIs_WithoutTouchingAComponent)
{
	put_quad("reload.geom.desc", 0.5f);
	entt::registry reg;
	add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto outside = toward_minus_z({ 1.5f, 0.f, 2.f });
	BOOST_REQUIRE(!scn::raycast_closest(reg, outside));
	BOOST_REQUIRE(scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f })));

	put("reload.geom.desc", test_util::quad_geometry_json(2.f));
	rs->signal_changed(geometry.get_tag());
	pump();
	BOOST_TEST(scn::raycast_closest(reg, outside).has_value());
	BOOST_TEST(scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f })).has_value());
}

BOOST_AUTO_TEST_CASE(ANaNWorldMatrix_IsSkipped_AndTheRestStillAnswers)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto broken = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	const auto fine = add_quad(reg, at({ 0.f, 0.f, -6.f }));
	reg.get<scn::world_transform>(broken).world[3][0] = std::numeric_limits<float>::quiet_NaN();
	const auto hit = scn::raycast_closest(reg, toward_minus_z({ 0.f, 0.f, 2.f }));
	BOOST_REQUIRE(hit);
	BOOST_TEST((hit->entity == fine));
}

BOOST_AUTO_TEST_CASE(ManyRandomQuads_TheTreesAnswerAsABruteForceLoopDoes)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	std::mt19937 random{ 1326 };
	std::uniform_real_distribution<float> position{ -20.f, 20.f };
	std::uniform_real_distribution<float> angle{ 0.f, 6.28f };
	std::uniform_real_distribution<float> scale{ 0.5f, 3.f };
	std::vector<glm::mat4> matrices;
	for (int i = 0; i < 300; ++i) {
		glm::mat4 matrix = at({ position(random), position(random), position(random) });
		matrix = glm::rotate(matrix, angle(random), glm::normalize(glm::vec3{ position(random) + 21.f, position(random) + 21.f, position(random) + 21.f }));
		matrix = glm::scale(matrix, glm::vec3{ scale(random) });
		matrices.push_back(matrix);
		add_quad(reg, matrix);
	}
	const glm::vec3 corners[4] = { { -0.5f, -0.5f, 0.f }, { 0.5f, -0.5f, 0.f }, { 0.5f, 0.5f, 0.f }, { -0.5f, 0.5f, 0.f } };
	const int triangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

	int hits = 0;
	for (int i = 0; i < 400; ++i) {
		const glm::vec3 origin{ position(random), position(random), position(random) };
		const glm::vec3 target{ position(random), position(random), position(random) };
		const scn::ray ray{ origin, glm::normalize(target - origin) };

		float expected = std::numeric_limits<float>::infinity();
		for (const glm::mat4& matrix : matrices) {
			for (const auto& triangle : triangles) {
				const glm::vec3 a = glm::vec3(matrix * glm::vec4(corners[triangle[0]], 1.f));
				const glm::vec3 b = glm::vec3(matrix * glm::vec4(corners[triangle[1]], 1.f));
				const glm::vec3 c = glm::vec3(matrix * glm::vec4(corners[triangle[2]], 1.f));
				if (const auto hit = ds::ray_triangle(ray.origin, ray.direction, a, b, c)) {
					expected = std::min(expected, hit->t);
				}
			}
		}
		const auto hit = scn::raycast_closest(reg, ray);
		BOOST_REQUIRE_MESSAGE(hit.has_value() == std::isfinite(expected), "ray " << i);
		if (hit) {
			++hits;
			BOOST_TEST(hit->distance == expected, boost::test_tools::tolerance(1e-3f));
		}
		BOOST_TEST(scn::raycast_any(reg, ray) == std::isfinite(expected));
	}
	BOOST_TEST(hits > 20);
}

BOOST_AUTO_TEST_CASE(TheRayThroughTheCenterOfTheImage_RunsAlongTheViewAxis)
{
	const glm::mat4 view = glm::lookAt(glm::vec3{ 0.f, 0.f, 5.f }, glm::vec3{ 0.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(60.f), 2.f, 0.1f, 100.f);
	const scn::ray center = scn::ray_from_screen(view, projection, { 100.f, 50.f }, { 200.f, 100.f });
	BOOST_TEST(center.direction.z == -1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(std::abs(center.direction.x) < 1e-4f);
	const scn::ray top_left = scn::ray_from_screen(view, projection, { 0.f, 0.f }, { 200.f, 100.f });
	BOOST_TEST(top_left.direction.x < 0.f);
	BOOST_TEST(top_left.direction.y > 0.f);
}

BOOST_AUTO_TEST_SUITE_END()
