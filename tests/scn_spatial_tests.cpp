#include <boost/test/unit_test.hpp>
#include "test_quad_geometry.hpp"
#include "test_scene_env.hpp"
#include <snk/common/ds/ds_intersect.hpp>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_spatial.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <entt/entt.hpp>
#include <algorithm>
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

BOOST_AUTO_TEST_CASE(ABatch_AnswersAsTheSyncQueriesDo_AndEchoesItsFrame)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto far_quad = add_quad(reg, at({ 0.f, 0.f, -8.f }));
	const auto near_quad = add_quad(reg, at({ 0.f, 0.f, -2.f }));
	scn::ray_request closest;
	closest.line = toward_minus_z({ 0.f, 0.f, 1.f });
	scn::ray_request any = closest;
	any.kind = scn::ray_query::any;
	scn::ray_request all = closest;
	all.kind = scn::ray_query::all;
	scn::ray_request miss = closest;
	miss.line = toward_minus_z({ 9.f, 0.f, 1.f });

	auto handle = scn::raycast_batch(tasks, reg, { closest, any, all, miss }, 77);
	pump();
	BOOST_REQUIRE(handle.status() == ds::task_status::ready);
	const scn::ray_batch& batch = handle.get();
	BOOST_TEST(batch.frame == 77u);
	BOOST_REQUIRE(batch.results.size() == 4u);
	BOOST_REQUIRE(batch.results[0].hits.size() == 1u);
	BOOST_TEST((batch.results[0].hits[0].entity == near_quad));
	BOOST_REQUIRE(batch.results[1].hits.size() == 1u);
	BOOST_REQUIRE(batch.results[2].hits.size() == 2u);
	BOOST_TEST((batch.results[2].hits[0].entity == near_quad));
	BOOST_TEST((batch.results[2].hits[1].entity == far_quad));
	BOOST_TEST(!batch.results[2].truncated);
	BOOST_TEST(batch.results[3].hits.empty());
}

BOOST_AUTO_TEST_CASE(ABatchOnRealWorkerThreads_AnswersAsOnTheManualPool_WhileTheWorldMovesOn)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	ds::task_system_config config;
	config.mode = ds::policy::threaded;
	ds::task_system workers{ config };

	scn::ray_request request;
	request.line = toward_minus_z({ 0.f, 0.f, 2.f });
	request.kind = scn::ray_query::all;
	std::vector<ds::task_handle<scn::ray_batch>> handles;
	for (int i = 0; i < 16; ++i) {
		handles.push_back(scn::raycast_batch(workers, reg, std::vector<scn::ray_request>(8, request), static_cast<std::uint64_t>(i)));
	}
	// The world changes while the workers read their snapshot.
	reg.get<scn::world_transform>(quad).world = at({ 10.f, 0.f, -3.f });
	const std::vector<entt::entity> moved{ quad };
	scn::spatial_world::attach(reg).entities_moved(reg, moved);

	for (std::size_t i = 0; i < handles.size(); ++i) {
		handles[i].wait();
		BOOST_REQUIRE(handles[i].status() == ds::task_status::ready);
		const scn::ray_batch& batch = handles[i].get();
		BOOST_TEST(batch.frame == static_cast<std::uint64_t>(i));
		BOOST_REQUIRE(batch.results.size() == 8u);
		for (const scn::ray_result& result : batch.results) {
			BOOST_TEST(result.hits.size() == 1u);
		}
	}
}

BOOST_AUTO_TEST_CASE(ABatch_Frame_N_DoesNotSeeTheMovesOfFrame_N_Plus_1)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	scn::ray_request request;
	request.line = toward_minus_z({ 0.f, 0.f, 2.f });

	auto first = scn::raycast_batch(tasks, reg, { request }, 1);

	reg.get<scn::world_transform>(quad).world = at({ 10.f, 0.f, -3.f });
	const std::vector<entt::entity> moved{ quad };
	scn::spatial_world::attach(reg).entities_moved(reg, moved);
	auto second = scn::raycast_batch(tasks, reg, { request }, 2);
	reg.destroy(quad);

	pump();
	BOOST_REQUIRE(first.status() == ds::task_status::ready);
	BOOST_REQUIRE(second.status() == ds::task_status::ready);
	BOOST_REQUIRE(first.get().results[0].hits.size() == 1u);
	BOOST_TEST((first.get().results[0].hits[0].entity == quad));
	BOOST_TEST(second.get().results[0].hits.empty());
}

BOOST_AUTO_TEST_CASE(BatchesHandedInWhileNothingChanges_ShareOneSnapshot_AChangeTakesANewOne)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -3.f }));
	scn::ray_request request;
	request.line = toward_minus_z({ 0.f, 0.f, 2.f });

	auto a = scn::raycast_batch(tasks, reg, { request }, 1);
	reg.remove<scn::renderable>(quad);
	auto b = scn::raycast_batch(tasks, reg, { request }, 1);
	pump();
	BOOST_TEST(a.get().results[0].hits.size() == 1u);
	BOOST_TEST(b.get().results[0].hits.empty());
}

BOOST_AUTO_TEST_CASE(AnAllRequest_IsCutAtMaxHits_NearestFirst_AndSaysSo)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	std::vector<entt::entity> quads;
	for (int i = 0; i < 5; ++i) {
		quads.push_back(add_quad(reg, at({ 0.f, 0.f, -2.f - static_cast<float>(i) })));
	}
	scn::ray_request request;
	request.line = toward_minus_z({ 0.f, 0.f, 1.f });
	request.kind = scn::ray_query::all;
	request.max_hits = 3;
	auto handle = scn::raycast_batch(tasks, reg, { request }, 5);
	pump();
	const scn::ray_result& result = handle.get().results[0];
	BOOST_REQUIRE(result.hits.size() == 3u);
	BOOST_TEST(result.truncated);
	for (std::size_t i = 0; i < 3; ++i) {
		BOOST_TEST((result.hits[i].entity == quads[i]));
	}
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

namespace {

// The frustum of a camera at `eye` looking at `target`, and an entity's world box as the engine takes it: the quad's box through its matrix.
glm::mat4 camera_view_projection(glm::vec3 eye, glm::vec3 target, float aspect = 2.f)
{
	return glm::perspective(glm::radians(60.f), aspect, 0.1f, 100.f) * glm::lookAt(eye, target, glm::vec3{ 0.f, 1.f, 0.f });
}

// Whether a box is wholly outside one of the clip planes (-w <= x, y, z <= w), from its eight corners in clip space: written out without
// ds::frustum_from or ds::test_frustum, so the test does not check the code under test with itself. It is the same conservative test the
// query documents (a box just outside an edge or a corner of the frustum is outside no single plane, so it counts as touching).
bool clip_space_outside(const glm::mat4& view_projection, const ds::aabb3& box)
{
	glm::vec4 corners[8];
	for (int i = 0; i < 8; ++i) {
		const glm::vec3 point{ (i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z };
		corners[i] = view_projection * glm::vec4(point, 1.f);
	}
	for (int axis = 0; axis < 3; ++axis) {
		bool all_below = true;
		bool all_above = true;
		for (const glm::vec4& clip : corners) {
			all_below = all_below && clip[axis] < -clip.w;
			all_above = all_above && clip[axis] > clip.w;
		}
		if (all_below || all_above) return true;
	}
	return false;
}

ds::aabb3 quad_box(const glm::mat4& matrix, float half = 0.5f)
{
	return ds::transformed(ds::aabb3{ glm::vec3{ -half, -half, 0.f }, glm::vec3{ half, half, 0.f } }, matrix);
}

}

BOOST_AUTO_TEST_CASE(AFrustumQuery_AnswersAsABruteForceTestOfTheBoxesDoes)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	std::mt19937 random{ 1327 };
	std::uniform_real_distribution<float> position{ -20.f, 20.f };
	std::uniform_real_distribution<float> angle{ 0.f, 6.28f };
	std::uniform_real_distribution<float> scale{ 0.5f, 3.f };
	std::vector<std::pair<entt::entity, glm::mat4>> quads;
	for (int i = 0; i < 300; ++i) {
		glm::mat4 matrix = at({ position(random), position(random), position(random) });
		matrix = glm::rotate(matrix, angle(random), glm::normalize(glm::vec3{ position(random) + 21.f, position(random) + 21.f, position(random) + 21.f }));
		matrix = glm::scale(matrix, glm::vec3{ scale(random) });
		quads.emplace_back(add_quad(reg, matrix), matrix);
	}
	int nonempty = 0;
	int partial = 0;
	for (int i = 0; i < 60; ++i) {
		const glm::vec3 eye{ position(random), position(random), position(random) };
		const glm::vec3 target{ position(random), position(random), position(random) };
		if (glm::length(target - eye) < 1.f) continue;
		const glm::mat4 view_projection = camera_view_projection(eye, target);
		std::vector<entt::entity> expected;
		for (const auto& [entity, matrix] : quads) {
			if (!clip_space_outside(view_projection, quad_box(matrix))) expected.push_back(entity);
		}
		std::sort(expected.begin(), expected.end());
		std::vector<entt::entity> found;
		scn::overlap_frustum(reg, view_projection, {}, found);
		BOOST_REQUIRE_MESSAGE((found == expected), "frustum " << i);
		nonempty += found.empty() ? 0 : 1;
		partial += (!found.empty() && found.size() < quads.size()) ? 1 : 0;
	}
	BOOST_TEST(nonempty > 10);
	BOOST_TEST(partial > 10);
}

BOOST_AUTO_TEST_CASE(AFrustumQuery_SkipsHiddenOtherLayersAndExcluded_UnlessAsked)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto shown = add_quad(reg, at({ -1.f, 0.f, -5.f }));
	const auto hidden = add_quad(reg, at({ 0.f, 0.f, -5.f }));
	reg.remove<scn::renderable>(hidden);
	const auto other_layer = add_quad(reg, at({ 1.f, 0.f, -5.f }));
	reg.emplace<scn::spatial_layers>(other_layer, scn::spatial_layers{ scn::spatial_mask{ scn::spatial_layer{ 5 } } });
	const auto excluded = add_quad(reg, at({ 2.f, 0.f, -5.f }));
	const glm::mat4 view_projection = camera_view_projection({ 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f });

	std::vector<entt::entity> found;
	scn::ray_filter filter;
	filter.mask = scn::spatial_mask{ scn::k_default_spatial_layer };
	const std::vector<entt::entity> out_of_the_way{ excluded };
	filter.exclude = out_of_the_way;
	scn::overlap_frustum(reg, view_projection, filter, found);
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST((found[0] == shown));

	found.clear();
	scn::ray_filter everything;
	everything.include_hidden = true;
	scn::overlap_frustum(reg, view_projection, everything, found);
	BOOST_TEST(found.size() == 4u);
}

BOOST_AUTO_TEST_CASE(AFrustumQuery_SeesAMovedEntityWhereItIs_AndNoLongerADestroyedOne)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -5.f }));
	const glm::mat4 view_projection = camera_view_projection({ 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f });
	std::vector<entt::entity> found;
	scn::overlap_frustum(reg, view_projection, {}, found);
	BOOST_TEST(found.size() == 1u);

	reg.get<scn::world_transform>(quad).world = at({ 0.f, 0.f, 50.f });
	scn::spatial_world::attach(reg).entities_moved(reg, std::vector<entt::entity>{ quad });
	found.clear();
	scn::overlap_frustum(reg, view_projection, {}, found);
	BOOST_TEST(found.empty());

	reg.get<scn::world_transform>(quad).world = at({ 0.f, 0.f, -5.f });
	scn::spatial_world::attach(reg).entities_moved(reg, std::vector<entt::entity>{ quad });
	reg.destroy(quad);
	found.clear();
	scn::overlap_frustum(reg, view_projection, {}, found);
	BOOST_TEST(found.empty());
}

BOOST_AUTO_TEST_CASE(AScreenRectangle_GivesTheFrustumOfThatPartOfTheImage)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto left = add_quad(reg, at({ -2.f, 0.f, -5.f }));
	const auto right = add_quad(reg, at({ 2.f, 0.f, -5.f }));
	const glm::mat4 view = glm::lookAt(glm::vec3{ 0.f }, glm::vec3{ 0.f, 0.f, -1.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(60.f), 2.f, 0.1f, 100.f);
	const glm::vec2 image{ 200.f, 100.f };
	const auto query = [&](glm::vec2 a, glm::vec2 b) {
		std::vector<entt::entity> found;
		scn::overlap_frustum(reg, scn::screen_rect_view_projection(view, projection, a, b, image), {}, found);
		return found;
	};

	BOOST_TEST((query({ 0.f, 0.f }, { 100.f, 100.f }) == std::vector<entt::entity>{ left }));
	BOOST_TEST((query({ 100.f, 0.f }, { 200.f, 100.f }) == std::vector<entt::entity>{ right }));
	BOOST_TEST((query({ 100.f, 100.f }, { 0.f, 0.f }) == std::vector<entt::entity>{ left }));
	BOOST_TEST(query({ 0.f, 0.f }, { 200.f, 100.f }).size() == 2u);
	BOOST_TEST(query({ 90.f, 40.f }, { 110.f, 60.f }).empty());
	BOOST_TEST(query({ 50.f, 50.f }, { 50.f, 90.f }).empty());
	BOOST_TEST(query({ 0.f, 0.f }, { 0.f, 0.f }).empty());
	BOOST_TEST((scn::screen_rect_view_projection(view, projection, { 0.f, 0.f }, { 10.f, 10.f }, { 0.f, 0.f }) == glm::mat4{ 0.f }));
}

BOOST_AUTO_TEST_CASE(AScreenRectangle_PinsTheYDirection_TopOfTheImageIsUp)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto top = add_quad(reg, at({ 0.f, 1.5f, -5.f }));
	const auto bottom = add_quad(reg, at({ 0.f, -1.5f, -5.f }));
	const glm::mat4 view = glm::lookAt(glm::vec3{ 0.f }, glm::vec3{ 0.f, 0.f, -1.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(60.f), 2.f, 0.1f, 100.f);
	const auto query = [&](glm::vec2 a, glm::vec2 b) {
		std::vector<entt::entity> found;
		scn::overlap_frustum(reg, scn::screen_rect_view_projection(view, projection, a, b, { 200.f, 100.f }), {}, found);
		return found;
	};
	BOOST_TEST((query({ 0.f, 0.f }, { 200.f, 50.f }) == std::vector<entt::entity>{ top }));
	BOOST_TEST((query({ 0.f, 50.f }, { 200.f, 100.f }) == std::vector<entt::entity>{ bottom }));
}

BOOST_AUTO_TEST_CASE(ACameraWithATinyNearPlaneAndAFarOne_StillFindsWhatItSees)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, -5.f }));
	const glm::mat4 view = glm::lookAt(glm::vec3{ 0.f }, glm::vec3{ 0.f, 0.f, -1.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projections[] = {
		glm::perspective(glm::radians(60.f), 2.f, 0.0001f, 5000.f),
		glm::perspective(glm::radians(60.f), 2.f, 0.0001f, 100000.f),
		glm::infinitePerspective(glm::radians(60.f), 2.f, 0.1f),
	};
	for (const glm::mat4& projection : projections) {
		std::vector<entt::entity> found;
		scn::overlap_frustum(reg, projection * view, {}, found);
		BOOST_TEST((found == std::vector<entt::entity>{ quad }));
		found.clear();
		scn::overlap_frustum(reg, scn::screen_rect_view_projection(view, projection, { 0.f, 0.f }, { 100.f, 100.f }, { 200.f, 100.f }), {}, found);
		BOOST_TEST((found == std::vector<entt::entity>{ quad }));
	}
}

BOOST_AUTO_TEST_CASE(AMatrixThatIsNoFrustum_AnswersNothing)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, at({ 0.f, 0.f, -5.f }));
	std::vector<entt::entity> found;
	scn::overlap_frustum(reg, glm::mat4{ 0.f }, {}, found);
	glm::mat4 broken = camera_view_projection({ 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f });
	broken[2][2] = std::numeric_limits<float>::quiet_NaN();
	scn::overlap_frustum(reg, broken, {}, found);
	BOOST_TEST(found.empty());
	entt::registry empty;
	scn::overlap_frustum(empty, camera_view_projection({ 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f }), {}, found);
	BOOST_TEST(found.empty());
}


namespace {

// Points of the quad through its matrix, on a grid: what the shape tests ask instead of the triangles the engine tests, so they do not
// check the code under test with itself. A sample is never nearer to a shape than the quad is, off by at most a grid step.
std::vector<glm::vec3> quad_samples(const glm::mat4& matrix, float half = 0.5f, int steps = 40)
{
	std::vector<glm::vec3> out;
	for (int i = 0; i <= steps; ++i) {
		for (int j = 0; j <= steps; ++j) {
			const glm::vec3 local{ -half + 2.f * half * static_cast<float>(i) / static_cast<float>(steps),
				-half + 2.f * half * static_cast<float>(j) / static_cast<float>(steps), 0.f };
			out.push_back(glm::vec3(matrix * glm::vec4(local, 1.f)));
		}
	}
	return out;
}

float distance_to_segment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
{
	const glm::vec3 ab = b - a;
	const float length_sq = glm::dot(ab, ab);
	const float along = length_sq > 0.f ? std::clamp(glm::dot(p - a, ab) / length_sq, 0.f, 1.f) : 0.f;
	return glm::distance(p, a + ab * along);
}

// How far a point is outside the shape (0 or less: inside), by the shape's own definition and nothing of the engine's.
float outside_by(const scn::shape& volume, const glm::vec3& p)
{
	switch (volume.kind) {
	case scn::shape_kind::sphere: return glm::distance(p, volume.center) - volume.radius;
	case scn::shape_kind::capsule: return distance_to_segment(p, volume.center, volume.end) - volume.radius;
	case scn::shape_kind::box: {
		const glm::vec3 local = glm::inverse(glm::mat3_cast(glm::normalize(volume.rotation))) * (p - volume.center);
		const glm::vec3 over = glm::max(glm::abs(local) - volume.half_extents, glm::vec3{ 0.f });
		return glm::length(over);
	}
	}
	return 0.f;
}

}

BOOST_AUTO_TEST_CASE(AShapeOverlap_AnswersAsASampledQuadDoes_ForSpheresCapsulesAndBoxes)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	std::mt19937 random{ 1328 };
	std::uniform_real_distribution<float> position{ -8.f, 8.f };
	std::uniform_real_distribution<float> angle{ 0.f, 6.28f };
	std::uniform_real_distribution<float> scale{ 0.5f, 3.f };
	std::uniform_real_distribution<float> size{ 0.2f, 2.f };
	std::vector<std::pair<entt::entity, std::vector<glm::vec3>>> quads;
	for (int i = 0; i < 120; ++i) {
		glm::mat4 matrix = at({ position(random), position(random), position(random) });
		matrix = glm::rotate(matrix, angle(random), glm::normalize(glm::vec3{ position(random) + 9.f, position(random) + 9.f, position(random) + 9.f }));
		matrix = glm::scale(matrix, glm::vec3{ scale(random), scale(random), 1.f });
		quads.emplace_back(add_quad(reg, matrix), quad_samples(matrix));
	}
	constexpr float k_slack = 0.12f;
	int met = 0;
	int missed = 0;
	for (int run = 0; run < 90; ++run) {
		const glm::vec3 centre{ position(random), position(random), position(random) };
		scn::shape volume;
		switch (run % 3) {
		case 0: volume = scn::shape::make_sphere(centre, size(random)); break;
		case 1: volume = scn::shape::make_capsule(centre, centre + glm::vec3{ position(random), position(random), position(random) } * 0.3f, size(random) * 0.5f); break;
		default:
			volume = scn::shape::make_box(centre, { size(random), size(random), size(random) },
				glm::angleAxis(angle(random), glm::normalize(glm::vec3{ position(random) + 9.f, position(random) + 9.f, position(random) + 9.f })));
			break;
		}
		std::vector<entt::entity> found;
		scn::overlap_shape(reg, volume, {}, found);
		BOOST_TEST(std::is_sorted(found.begin(), found.end()));
		for (const auto& [entity, samples] : quads) {
			float nearest = std::numeric_limits<float>::max();
			for (const glm::vec3& sample : samples) nearest = std::min(nearest, outside_by(volume, sample));
			const bool reported = std::find(found.begin(), found.end(), entity) != found.end();
			BOOST_TEST_CONTEXT("run " << run << " quad " << static_cast<std::uint32_t>(entity)) {
				if (nearest <= 0.f) BOOST_TEST(reported);
				if (nearest > k_slack) BOOST_TEST(!reported);
			}
			met += reported ? 1 : 0;
			missed += reported ? 0 : 1;
		}
	}
	BOOST_TEST(met > 20);
	BOOST_TEST(missed > 20);
}

BOOST_AUTO_TEST_CASE(AShapeOverlap_UsesTheStretchedMeshNotAStretchedShape)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto wide = add_quad(reg, glm::scale(at({ 0.f, 0.f, 0.f }), glm::vec3{ 4.f, 1.f, 1.f }));
	std::vector<entt::entity> found;
	// The quad's edge is at x = 2 now: a unit sphere at x = 2.8 reaches it, one at x = 3.2 does not.
	scn::overlap_shape(reg, scn::shape::make_sphere({ 2.8f, 0.f, 0.f }, 1.f), {}, found);
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST((found[0] == wide));
	found.clear();
	scn::overlap_shape(reg, scn::shape::make_sphere({ 3.2f, 0.f, 0.f }, 1.f), {}, found);
	BOOST_TEST(found.empty());
	found.clear();
	scn::overlap_shape(reg, scn::shape::make_sphere({ 0.f, 0.f, 2.f }, 1.f), {}, found);
	BOOST_TEST(found.empty());
	scn::overlap_shape(reg, scn::shape::make_box({ 0.f, 0.f, 0.5f }, { 3.f, 0.2f, 0.5f }), {}, found);
	BOOST_TEST(found.size() == 1u);
}

BOOST_AUTO_TEST_CASE(AShapeOverlap_SkipsHiddenOtherLayersAndExcluded_SeesAMoveAndADestroy)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto shown = add_quad(reg, at({ -1.f, 0.f, 0.f }));
	const auto hidden = add_quad(reg, at({ 0.f, 0.f, 0.f }));
	reg.remove<scn::renderable>(hidden);
	const auto other_layer = add_quad(reg, at({ 1.f, 0.f, 0.f }));
	reg.emplace<scn::spatial_layers>(other_layer, scn::spatial_layers{ scn::spatial_mask{ scn::spatial_layer{ 5 } } });
	const auto excluded = add_quad(reg, at({ 2.f, 0.f, 0.f }));
	const scn::shape around = scn::shape::make_box({ 0.5f, 0.f, 0.f }, { 4.f, 1.f, 1.f });

	std::vector<entt::entity> found;
	scn::ray_filter filter;
	filter.mask = scn::spatial_mask{ scn::k_default_spatial_layer };
	const std::vector<entt::entity> out_of_the_way{ excluded };
	filter.exclude = out_of_the_way;
	scn::overlap_shape(reg, around, filter, found);
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST((found[0] == shown));

	found.clear();
	scn::ray_filter everything;
	everything.include_hidden = true;
	scn::overlap_shape(reg, around, everything, found);
	BOOST_TEST(found.size() == 4u);

	reg.get<scn::world_transform>(shown).world = at({ 50.f, 0.f, 0.f });
	const entt::entity moved[] = { shown };
	scn::spatial_world::attach(reg).entities_moved(reg, moved);
	reg.destroy(excluded);
	found.clear();
	scn::overlap_shape(reg, around, everything, found);
	BOOST_TEST(found.size() == 2u);
}

BOOST_AUTO_TEST_CASE(ABadShape_OrAPointThatIsNaN_AnswersNothing)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, at({ 0.f, 0.f, 0.f }));
	const float nan = std::nanf("");
	std::vector<entt::entity> found;
	scn::overlap_shape(reg, scn::shape::make_sphere({ nan, 0.f, 0.f }, 1.f), {}, found);
	scn::overlap_shape(reg, scn::shape::make_sphere({ 0.f, 0.f, 0.f }, -1.f), {}, found);
	scn::overlap_shape(reg, scn::shape::make_sphere({ 0.f, 0.f, 0.f }, nan), {}, found);
	scn::overlap_shape(reg, scn::shape::make_capsule({ 0.f, 0.f, 0.f }, { 0.f, std::numeric_limits<float>::infinity(), 0.f }, 1.f), {}, found);
	scn::overlap_shape(reg, scn::shape::make_box({ 0.f, 0.f, 0.f }, { 1.f, -1.f, 1.f }), {}, found);
	scn::overlap_shape(reg, scn::shape::make_box({ 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f }, glm::quat{ 0.f, 0.f, 0.f, 0.f }), {}, found);
	scn::overlap_point(reg, { nan, 0.f, 0.f }, {}, found);
	BOOST_TEST(found.empty());
	BOOST_TEST(!scn::closest_point(reg, { nan, 0.f, 0.f }).has_value());
	BOOST_TEST(scn::closest_point(reg, { 0.f, 0.f, 1.f }).has_value());
}

BOOST_AUTO_TEST_CASE(APointQuery_FindsTheEntitiesWhoseWorldBoxHoldsIt)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto flat = add_quad(reg, at({ 0.f, 0.f, 0.f }));
	const auto turned = add_quad(reg, glm::rotate(at({ 10.f, 0.f, 0.f }), glm::radians(45.f), glm::vec3{ 0.f, 0.f, 1.f }));
	std::vector<entt::entity> found;
	scn::overlap_point(reg, { 0.2f, 0.2f, 0.f }, {}, found);
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST((found[0] == flat));
	found.clear();
	scn::overlap_point(reg, { 0.2f, 0.2f, 0.01f }, {}, found);
	BOOST_TEST(found.empty());
	// A box of a turned quad is bigger than the quad: its corner counts, as the frustum query's does.
	scn::overlap_point(reg, { 10.6f, 0.6f, 0.f }, {}, found);
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST((found[0] == turned));
	found.clear();
	scn::overlap_point(reg, { 5.f, 0.f, 0.f }, {}, found);
	BOOST_TEST(found.empty());
}

BOOST_AUTO_TEST_CASE(TheClosestPoint_IsOnTheSurface_WithDistanceNormalAndTriangle)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto quad = add_quad(reg, at({ 0.f, 0.f, 0.f }));
	const auto above = scn::closest_point(reg, { 0.2f, 0.1f, 3.f });
	BOOST_REQUIRE(above.has_value());
	BOOST_TEST((above->entity == quad));
	BOOST_TEST(above->distance == 3.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(above->point.x == 0.2f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(above->point.z == 0.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(above->normal.z == 1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(above->triangle < 2u);

	const auto beside = scn::closest_point(reg, { 2.5f, 0.f, 0.f });
	BOOST_REQUIRE(beside.has_value());
	BOOST_TEST(beside->distance == 2.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(beside->point.x == 0.5f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(beside->normal.x == 1.f, boost::test_tools::tolerance(1e-4f));

	const auto on = scn::closest_point(reg, { 0.1f, 0.1f, 0.f });
	BOOST_REQUIRE(on.has_value());
	BOOST_TEST(on->distance == 0.f);
	BOOST_TEST(on->normal.z == 1.f, boost::test_tools::tolerance(1e-4f));

	const auto below = scn::closest_point(reg, { 0.f, 0.f, -2.f });
	BOOST_REQUIRE(below.has_value());
	BOOST_TEST(below->normal.z == -1.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(TheClosestPoint_TakesTheNearerEntity_TheSearchRadiusAndTheFilter)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	const auto near_quad = add_quad(reg, at({ 0.f, 0.f, 1.f }));
	const auto far_quad = add_quad(reg, at({ 0.f, 0.f, 6.f }));
	const auto best = scn::closest_point(reg, { 0.f, 0.f, 3.f });
	BOOST_REQUIRE(best.has_value());
	BOOST_TEST((best->entity == near_quad));
	BOOST_TEST(best->distance == 2.f, boost::test_tools::tolerance(1e-4f));

	scn::ray_filter close;
	close.max_distance = 1.5f;
	BOOST_TEST(!scn::closest_point(reg, { 0.f, 0.f, 3.f }, close).has_value());
	close.max_distance = 2.f;
	BOOST_TEST(scn::closest_point(reg, { 0.f, 0.f, 3.f }, close).has_value());

	const std::vector<entt::entity> skipped{ near_quad };
	scn::ray_filter without;
	without.exclude = skipped;
	const auto other = scn::closest_point(reg, { 0.f, 0.f, 3.f }, without);
	BOOST_REQUIRE(other.has_value());
	BOOST_TEST((other->entity == far_quad));

	reg.remove<scn::renderable>(far_quad);
	BOOST_TEST(!scn::closest_point(reg, { 0.f, 0.f, 3.f }, without).has_value());
	without.include_hidden = true;
	BOOST_TEST(scn::closest_point(reg, { 0.f, 0.f, 3.f }, without).has_value());

	// Two at the same distance: the lower entity.
	const auto twin_a = add_quad(reg, at({ 10.f, 0.f, 1.f }));
	const auto twin_b = add_quad(reg, at({ 10.f, 0.f, -1.f }));
	const auto tie = scn::closest_point(reg, { 10.f, 0.f, 0.f });
	BOOST_REQUIRE(tie.has_value());
	BOOST_TEST((tie->entity == std::min(twin_a, twin_b)));
}

BOOST_AUTO_TEST_CASE(TheClosestPoint_MeasuresInTheWorld_WhenTheMatrixStretchesTheMesh)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	add_quad(reg, glm::scale(at({ 0.f, 0.f, 0.f }), glm::vec3{ 4.f, 1.f, 1.f }));
	// The quad spans x in [-2, 2] now: from (3, 0, 1) the nearest point is its edge (2, 0, 0).
	const auto hit = scn::closest_point(reg, { 3.f, 0.f, 1.f });
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->point.x == 2.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(hit->distance == std::sqrt(2.f), boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(TheClosestPoint_AnswersAsASampledQuadDoes_ForRandomQuadsAndPoints)
{
	put_quad("quad.geom.desc");
	entt::registry reg;
	std::mt19937 random{ 13280 };
	std::uniform_real_distribution<float> position{ -8.f, 8.f };
	std::uniform_real_distribution<float> angle{ 0.f, 6.28f };
	std::uniform_real_distribution<float> scale{ 0.5f, 3.f };
	std::vector<std::vector<glm::vec3>> quads;
	for (int i = 0; i < 80; ++i) {
		glm::mat4 matrix = at({ position(random), position(random), position(random) });
		matrix = glm::rotate(matrix, angle(random), glm::normalize(glm::vec3{ position(random) + 9.f, position(random) + 9.f, position(random) + 9.f }));
		matrix = glm::scale(matrix, glm::vec3{ scale(random), scale(random), 1.f });
		add_quad(reg, matrix);
		quads.push_back(quad_samples(matrix));
	}
	for (int run = 0; run < 40; ++run) {
		const glm::vec3 p{ position(random), position(random), position(random) };
		float sampled = std::numeric_limits<float>::max();
		for (const auto& samples : quads) {
			for (const glm::vec3& sample : samples) sampled = std::min(sampled, glm::distance(p, sample));
		}
		const auto hit = scn::closest_point(reg, p);
		BOOST_TEST_CONTEXT("point " << run) {
			BOOST_REQUIRE(hit.has_value());
			BOOST_TEST(hit->distance <= sampled + 1e-3f);
			BOOST_TEST(hit->distance >= sampled - 0.12f);
			BOOST_TEST(glm::distance(p, hit->point) == hit->distance, boost::test_tools::tolerance(1e-3f));
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
