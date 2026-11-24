#include <boost/test/unit_test.hpp>
#include "edt_mesh_gizmo.h"
#include <cmath>
#include <optional>
#include <numbers>
#include <glm/gtc/matrix_transform.hpp>

namespace {

using namespace edt::mesh;

struct gizmo_fixture
{
	gizmo_fixture()
		: screen(glm::perspective(glm::radians(60.f), 1.f, 0.1f, 100.f) * glm::lookAt(glm::vec3(2.f, 2.f, 6.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f)),
			glm::vec2(0.f), glm::vec2(800.f))
	{
	}

	static bool close_to(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) { return glm::length(a - b) < eps; }

	gizmo_layout layout(gizmo_tool tool)
	{
		const auto made = make_gizmo_layout(screen, frame, tool, metrics);
		BOOST_REQUIRE(made.has_value());
		return *made;
	}

	glm::vec2 pixel(const glm::vec3& world) const { return *screen.project(world); }

	screen_view screen;
	pivot_frame frame;
	gizmo_metrics metrics;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshGizmoTests, gizmo_fixture)

BOOST_AUTO_TEST_CASE(Layout_HasTheShapesOfItsTool)
{
	BOOST_TEST(layout(gizmo_tool::move).shapes.size() == 7u);
	BOOST_TEST(layout(gizmo_tool::rotate).shapes.size() == 4u);
	BOOST_TEST(layout(gizmo_tool::scale).shapes.size() == 4u);
}

BOOST_AUTO_TEST_CASE(Layout_APivotBehindTheCamera_IsNothing)
{
	pivot_frame behind;
	behind.origin = glm::vec3(20.f, 20.f, 60.f);
	BOOST_TEST(!make_gizmo_layout(screen, behind, gizmo_tool::move, metrics).has_value());
}

BOOST_AUTO_TEST_CASE(Layout_AnAxisPointingAtTheCamera_FadesOut)
{
	pivot_frame facing;
	const glm::vec3 to_camera = glm::normalize(glm::vec3(2.f, 2.f, 6.f));
	facing.z = to_camera;
	facing.x = glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), to_camera));
	facing.y = glm::cross(to_camera, facing.x);
	const auto made = make_gizmo_layout(screen, facing, gizmo_tool::move, metrics);
	BOOST_REQUIRE(made.has_value());
	BOOST_TEST(made->find(gizmo_part::z)->visibility < 0.01f);
	BOOST_TEST(made->find(gizmo_part::x)->visibility > 0.99f);
	BOOST_TEST(made->find(gizmo_part::xy)->visibility > 0.99f);
	BOOST_TEST(made->find(gizmo_part::yz)->visibility < 0.01f);
	BOOST_TEST(made->find(gizmo_part::zx)->visibility < 0.01f);
	BOOST_TEST((pick_gizmo_part(*made, made->find(gizmo_part::z)->anchor, metrics) != gizmo_part::z));
}

BOOST_AUTO_TEST_CASE(Pick_TheTipOfAnArrow_TheCentre_AndEmptySpace)
{
	const gizmo_layout move = layout(gizmo_tool::move);
	BOOST_TEST((pick_gizmo_part(move, move.find(gizmo_part::x)->anchor, metrics) == gizmo_part::x));
	BOOST_TEST((pick_gizmo_part(move, move.find(gizmo_part::y)->points[1], metrics) == gizmo_part::y));
	BOOST_TEST((pick_gizmo_part(move, move.origin, metrics) == gizmo_part::centre));
	BOOST_TEST((pick_gizmo_part(move, glm::vec2(5.f, 5.f), metrics) == gizmo_part::none));
}

BOOST_AUTO_TEST_CASE(Pick_APlaneHandle_IsTakenInsideItsQuad)
{
	const gizmo_layout move = layout(gizmo_tool::move);
	const gizmo_shape* plane = move.find(gizmo_part::xy);
	BOOST_REQUIRE(plane && plane->points.size() == 4u);
	const glm::vec2 middle = (plane->points[0] + plane->points[2]) * 0.5f;
	BOOST_TEST((pick_gizmo_part(move, middle, metrics) == gizmo_part::xy));
}

BOOST_AUTO_TEST_CASE(Pick_ARingAndTheScreenRing)
{
	const gizmo_layout rotate = layout(gizmo_tool::rotate);
	BOOST_TEST((pick_gizmo_part(rotate, rotate.find(gizmo_part::y)->points[5], metrics) == gizmo_part::y));
	const glm::vec2 on_screen_ring = rotate.origin + glm::vec2(rotate.screen_ring_radius, 0.f);
	BOOST_TEST((pick_gizmo_part(rotate, on_screen_ring, metrics) == gizmo_part::screen));
}

BOOST_AUTO_TEST_CASE(MoveDrag_AlongAnAxis_MovesByTheDistanceTheMouseTravelled)
{
	const gizmo_layout move = layout(gizmo_tool::move);
	const glm::vec2 press = pixel(glm::vec3(1.f, 0.f, 0.f));
	auto drag = gizmo_drag::begin(screen, move, gizmo_part::x, press);
	BOOST_REQUIRE(drag.has_value());
	const auto delta = drag->update(screen, pixel(glm::vec3(2.5f, 0.f, 0.f)), {});
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(close_to(delta->translation, glm::vec3(1.5f, 0.f, 0.f)));
	BOOST_TEST(close_to(glm::vec3(delta->matrix * glm::vec4(0.f, 1.f, 0.f, 1.f)), glm::vec3(1.5f, 1.f, 0.f)));
}

BOOST_AUTO_TEST_CASE(MoveDrag_WithASnapStep_LandsOnTheStep)
{
	const gizmo_layout move = layout(gizmo_tool::move);
	auto drag = gizmo_drag::begin(screen, move, gizmo_part::x, pixel(glm::vec3(1.f, 0.f, 0.f)));
	BOOST_REQUIRE(drag.has_value());
	gizmo_snap snap;
	snap.move = 0.5f;
	const auto delta = drag->update(screen, pixel(glm::vec3(2.37f, 0.f, 0.f)), snap);
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(delta->translation.x == 1.5f, boost::test_tools::tolerance(1e-3f));
}

BOOST_AUTO_TEST_CASE(MoveDrag_InAPlane_KeepsTheThirdAxisStill)
{
	const gizmo_layout move = layout(gizmo_tool::move);
	auto drag = gizmo_drag::begin(screen, move, gizmo_part::xy, pixel(glm::vec3(0.4f, 0.4f, 0.f)));
	BOOST_REQUIRE(drag.has_value());
	const auto delta = drag->update(screen, pixel(glm::vec3(1.4f, 0.9f, 0.f)), {});
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(close_to(delta->translation, glm::vec3(1.f, 0.5f, 0.f)));
}

BOOST_AUTO_TEST_CASE(RotateDrag_OnARing_TurnsByTheAngleBetweenThePoints)
{
	const gizmo_layout rotate = layout(gizmo_tool::rotate);
	const gizmo_shape* ring = rotate.find(gizmo_part::y);
	BOOST_REQUIRE(ring && ring->points.size() == 48u);
	auto drag = gizmo_drag::begin(screen, rotate, gizmo_part::y, ring->points[0]);
	BOOST_REQUIRE(drag.has_value());
	const auto delta = drag->update(screen, ring->points[12], {});
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(delta->angle_radians == std::numbers::pi_v<float> / 2.f, boost::test_tools::tolerance(1e-2f));
}

BOOST_AUTO_TEST_CASE(RotateDrag_WithASnapStep_RoundsTheAngle)
{
	const gizmo_layout rotate = layout(gizmo_tool::rotate);
	const gizmo_shape* ring = rotate.find(gizmo_part::y);
	auto drag = gizmo_drag::begin(screen, rotate, gizmo_part::y, ring->points[0]);
	BOOST_REQUIRE(drag.has_value());
	gizmo_snap snap;
	snap.rotate_radians = std::numbers::pi_v<float> / 12.f;
	const auto delta = drag->update(screen, ring->points[7], snap);
	BOOST_REQUIRE(delta.has_value());
	const float steps = delta->angle_radians / snap.rotate_radians;
	BOOST_TEST(std::abs(steps - std::round(steps)) < 1e-3f);
}

BOOST_AUTO_TEST_CASE(ScaleDrag_OnAnAxis_ScalesOnlyThatAxis_AndTheCentreAllOfThem)
{
	const gizmo_layout scale = layout(gizmo_tool::scale);
	const glm::vec2 press = scale.origin + glm::vec2(40.f, 0.f);
	auto axis = gizmo_drag::begin(screen, scale, gizmo_part::x, press);
	BOOST_REQUIRE(axis.has_value());
	const auto along = axis->update(screen, scale.origin + glm::vec2(80.f, 0.f), {});
	BOOST_REQUIRE(along.has_value());
	BOOST_TEST(close_to(along->factors, glm::vec3(2.f, 1.f, 1.f)));

	auto all = gizmo_drag::begin(screen, scale, gizmo_part::centre, press);
	BOOST_REQUIRE(all.has_value());
	const auto uniform = all->update(screen, scale.origin + glm::vec2(20.f, 0.f), {});
	BOOST_REQUIRE(uniform.has_value());
	BOOST_TEST(close_to(uniform->factors, glm::vec3(0.5f)));
}

BOOST_AUTO_TEST_CASE(ScaleDrag_NeverGoesToZeroOrBelow)
{
	const gizmo_layout scale = layout(gizmo_tool::scale);
	auto drag = gizmo_drag::begin(screen, scale, gizmo_part::centre, scale.origin + glm::vec2(40.f, 0.f));
	BOOST_REQUIRE(drag.has_value());
	const auto delta = drag->update(screen, scale.origin, {});
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(delta->factors.x > 0.f);
}

BOOST_AUTO_TEST_CASE(Numeric_MoveTakesThePivotToTheValue_RotateTurnsInDegrees_ScaleKeepsThePivot)
{
	pivot_frame at;
	at.origin = glm::vec3(1.f, 2.f, 3.f);
	BOOST_TEST(close_to(numeric_defaults(gizmo_tool::move, at), at.origin));
	BOOST_TEST(close_to(numeric_defaults(gizmo_tool::scale, at), glm::vec3(1.f)));
	BOOST_TEST(close_to(glm::vec3(numeric_matrix(gizmo_tool::move, at, glm::vec3(4.f, 2.f, 3.f)) * glm::vec4(at.origin, 1.f)), glm::vec3(4.f, 2.f, 3.f)));
	BOOST_TEST(close_to(glm::vec3(numeric_matrix(gizmo_tool::rotate, at, glm::vec3(0.f, 0.f, 90.f)) * glm::vec4(2.f, 2.f, 3.f, 1.f)), glm::vec3(1.f, 3.f, 3.f)));
	BOOST_TEST(close_to(glm::vec3(numeric_matrix(gizmo_tool::scale, at, glm::vec3(2.f, 2.f, 2.f)) * glm::vec4(at.origin, 1.f)), at.origin));
	BOOST_TEST(close_to(glm::vec3(numeric_matrix(gizmo_tool::scale, at, glm::vec3(2.f, 1.f, 1.f)) * glm::vec4(2.f, 2.f, 3.f, 1.f)), glm::vec3(3.f, 2.f, 3.f)));
}

BOOST_AUTO_TEST_CASE(MoveDrag_AlongAnAxisThatPointsAtTheCamera_IsNothing)
{
	const screen_view along(glm::perspective(glm::radians(60.f), 1.f, 0.1f, 100.f) * glm::lookAt(glm::vec3(6.f, 0.f, 0.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f)),
		glm::vec2(0.f), glm::vec2(800.f));
	const auto made = make_gizmo_layout(along, frame, gizmo_tool::move, metrics);
	BOOST_REQUIRE(made.has_value());
	BOOST_TEST(!gizmo_drag::begin(along, *made, gizmo_part::x, made->origin + glm::vec2(30.f, 0.f)).has_value());
}

BOOST_AUTO_TEST_CASE(Pick_ARingSeenEdgeOn_IsNotPicked)
{
	const screen_view front(glm::perspective(glm::radians(60.f), 1.f, 0.1f, 100.f) * glm::lookAt(glm::vec3(0.f, 0.f, 6.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f)),
		glm::vec2(0.f), glm::vec2(800.f));
	const auto made = make_gizmo_layout(front, frame, gizmo_tool::rotate, metrics);
	BOOST_REQUIRE(made.has_value());
	BOOST_TEST(made->find(gizmo_part::x)->visibility < 0.01f);
	BOOST_TEST(made->find(gizmo_part::z)->visibility > 0.99f);
	BOOST_TEST((pick_gizmo_part(*made, made->origin + glm::vec2(0.f, metrics.arm * 0.5f), metrics) == gizmo_part::none));
}

BOOST_AUTO_TEST_CASE(RotateDrag_PastAHalfTurn_AddsTheTurnsUp)
{
	const gizmo_layout rotate = layout(gizmo_tool::rotate);
	const gizmo_shape* ring = rotate.find(gizmo_part::y);
	auto drag = gizmo_drag::begin(screen, rotate, gizmo_part::y, ring->points[0]);
	BOOST_REQUIRE(drag.has_value());
	std::optional<gizmo_delta> delta;
	for (int step = 6; step <= 36; step += 6) {
		delta = drag->update(screen, ring->points[static_cast<std::size_t>(step)], {});
		BOOST_REQUIRE(delta.has_value());
	}
	BOOST_TEST(delta->angle_radians == 3.f * std::numbers::pi_v<float> / 2.f, boost::test_tools::tolerance(1e-2f));
}

BOOST_AUTO_TEST_CASE(ScaleDrag_StartedNextToThePivot_DoesNotBlowUp)
{
	const gizmo_layout scale = layout(gizmo_tool::scale);
	auto drag = gizmo_drag::begin(screen, scale, gizmo_part::centre, scale.origin + glm::vec2(1.f, 0.f));
	BOOST_REQUIRE(drag.has_value());
	const auto delta = drag->update(screen, scale.origin + glm::vec2(metrics.arm, 0.f), {});
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(delta->factors.x < 5.f);
	BOOST_TEST(delta->factors.x > 1.f);
}

BOOST_AUTO_TEST_CASE(ScaleDrag_WithASnapStep_NeverRoundsToZero)
{
	const gizmo_layout scale = layout(gizmo_tool::scale);
	auto drag = gizmo_drag::begin(screen, scale, gizmo_part::centre, scale.origin + glm::vec2(metrics.arm, 0.f));
	BOOST_REQUIRE(drag.has_value());
	gizmo_snap snap;
	snap.scale = 0.1f;
	const auto delta = drag->update(screen, scale.origin + glm::vec2(metrics.arm * 0.04f, 0.f), snap);
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(delta->factors.x >= 0.1f - 1e-5f);
}

BOOST_AUTO_TEST_CASE(MoveDrag_ASnapToTheNegativeSide_RoundsTheDelta)
{
	const gizmo_layout move = layout(gizmo_tool::move);
	auto drag = gizmo_drag::begin(screen, move, gizmo_part::x, pixel(glm::vec3(1.f, 0.f, 0.f)));
	BOOST_REQUIRE(drag.has_value());
	gizmo_snap snap;
	snap.move = 0.5f;
	const auto delta = drag->update(screen, pixel(glm::vec3(-0.37f, 0.f, 0.f)), snap);
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(delta->translation.x == -1.5f, boost::test_tools::tolerance(1e-3f));
}

BOOST_AUTO_TEST_CASE(MoveDrag_InANormalFrame_FollowsTheFramesAxis)
{
	pivot_frame turned;
	turned.x = glm::vec3(0.f, 1.f, 0.f);
	turned.y = glm::vec3(0.f, 0.f, 1.f);
	turned.z = glm::vec3(1.f, 0.f, 0.f);
	const auto made = make_gizmo_layout(screen, turned, gizmo_tool::move, metrics);
	BOOST_REQUIRE(made.has_value());
	auto drag = gizmo_drag::begin(screen, *made, gizmo_part::x, pixel(glm::vec3(0.f, 1.f, 0.f)));
	BOOST_REQUIRE(drag.has_value());
	const auto delta = drag->update(screen, pixel(glm::vec3(0.f, 2.5f, 0.f)), {});
	BOOST_REQUIRE(delta.has_value());
	BOOST_TEST(close_to(delta->translation, glm::vec3(0.f, 1.5f, 0.f)));
}

BOOST_AUTO_TEST_CASE(Numeric_RotateAppliesXThenYThenZ)
{
	pivot_frame at;
	BOOST_TEST(close_to(glm::vec3(numeric_matrix(gizmo_tool::rotate, at, glm::vec3(90.f, 0.f, 90.f)) * glm::vec4(0.f, 1.f, 0.f, 1.f)), glm::vec3(0.f, 0.f, 1.f)));
}

BOOST_AUTO_TEST_CASE(Numeric_AScaleOfZero_IsRefused_ANegativeOneMirrors)
{
	BOOST_TEST(!numeric_problem(gizmo_tool::scale, glm::vec3(1.f, 0.f, 1.f)).empty());
	BOOST_TEST(numeric_problem(gizmo_tool::scale, glm::vec3(-1.f, 1.f, 1.f)).empty());
	BOOST_TEST(numeric_problem(gizmo_tool::move, glm::vec3(0.f)).empty());
	BOOST_TEST(!numeric_problem(gizmo_tool::move, glm::vec3(std::nanf(""), 0.f, 0.f)).empty());
}

BOOST_AUTO_TEST_SUITE_END()
