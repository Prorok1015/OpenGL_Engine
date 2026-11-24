#include <boost/test/unit_test.hpp>
#include "edt_mesh_diff.h"
#include "edt_mesh_shapes.h"
#include "edt_mesh_transform.h"
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace {

using namespace edt::mesh;

struct cube_fixture
{
	cube_fixture()
	{
		shape_result result = make_shape(box_params{});
		BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
		mesh = std::move(*result.mesh);
		for (const auto& entry : mesh.vertices()) {
			selection.apply_vertex(entry.first, select_op::add);
		}
	}

	static bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; }

	poly_mesh mesh;
	mesh_selection selection;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshTransformTests, cube_fixture)

BOOST_AUTO_TEST_CASE(SelectionFrame_OfAnEmptySelection_IsNothing)
{
	BOOST_TEST(!selection_frame(mesh, mesh_selection{}, pivot_mode::median, gizmo_orientation::world).has_value());
}

BOOST_AUTO_TEST_CASE(SelectionFrame_OfTheWholeCube_IsCentredAtTheMiddle)
{
	const auto frame = selection_frame(mesh, selection, pivot_mode::median, gizmo_orientation::world);
	BOOST_REQUIRE(frame.has_value());
	BOOST_TEST(near(frame->origin, glm::vec3(0.f)));
}

BOOST_AUTO_TEST_CASE(Translate_ThenBackToZero_ReturnsTheSnapshotPositions)
{
	const auto snapshot = capture_vertices(mesh, selected_vertices(mesh, selection));
	const std::size_t count = apply_transform(mesh, snapshot, translation_matrix(glm::vec3(1.f, 2.f, 3.f)));
	BOOST_TEST(count == snapshot.positions.size());
	for (const auto& [id, position] : snapshot.positions) {
		BOOST_TEST(near(mesh.vertices().at(id).position, position + glm::vec3(1.f, 2.f, 3.f)));
	}
	apply_transform(mesh, snapshot, translation_matrix(glm::vec3(0.f)));
	for (const auto& [id, position] : snapshot.positions) {
		BOOST_TEST(near(mesh.vertices().at(id).position, position));
	}
}

BOOST_AUTO_TEST_CASE(Rotate_FourQuarterTurns_IsTheIdentity)
{
	const glm::vec3 pivot(0.3f, 0.f, 0.f);
	const glm::mat4 quarter = rotation_matrix(pivot, glm::vec3(0.f, 1.f, 0.f), std::numbers::pi_v<float> / 2.f);
	const glm::mat4 full = quarter * quarter * quarter * quarter;
	const glm::vec3 point(1.f, 2.f, 3.f);
	BOOST_TEST(near(glm::vec3(full * glm::vec4(point, 1.f)), point));
}

BOOST_AUTO_TEST_CASE(Rotate_AboutZ_TurnsXTowardsY_AndKeepsThePivot)
{
	const glm::mat4 turn = rotation_matrix(glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f, 0.f, 1.f), std::numbers::pi_v<float> / 2.f);
	BOOST_TEST(near(glm::vec3(turn * glm::vec4(2.f, 0.f, 0.f, 1.f)), glm::vec3(1.f, 1.f, 0.f)));
	BOOST_TEST(near(glm::vec3(turn * glm::vec4(1.f, 0.f, 0.f, 1.f)), glm::vec3(1.f, 0.f, 0.f)));
	BOOST_TEST(near(glm::vec3(rotation_matrix(glm::vec3(0.f), glm::vec3(0.f), 1.f) * glm::vec4(1.f, 2.f, 3.f, 1.f)), glm::vec3(1.f, 2.f, 3.f)));
}

BOOST_AUTO_TEST_CASE(Scale_KeepsThePivotAndScalesAlongTheFramesAxes)
{
	pivot_frame frame;
	frame.origin = glm::vec3(1.f, 0.f, 0.f);
	const glm::mat4 scale = scale_matrix(frame, glm::vec3(2.f, 1.f, 1.f));
	BOOST_TEST(near(glm::vec3(scale * glm::vec4(frame.origin, 1.f)), frame.origin));
	BOOST_TEST(near(glm::vec3(scale * glm::vec4(2.f, 1.f, 0.f, 1.f)), glm::vec3(3.f, 1.f, 0.f)));
}

BOOST_AUTO_TEST_CASE(ApplyTransform_ANonFiniteResult_LeavesTheVertexWhereItWas)
{
	const auto snapshot = capture_vertices(mesh, selected_vertices(mesh, selection));
	const float inf = std::numeric_limits<float>::infinity();
	const std::size_t count = apply_transform(mesh, snapshot, translation_matrix(glm::vec3(inf, 0.f, 0.f)));
	BOOST_TEST(count == 0u);
	for (const auto& [id, position] : snapshot.positions) {
		BOOST_TEST(near(mesh.vertices().at(id).position, position));
	}
}

BOOST_AUTO_TEST_CASE(SnapToStep_RoundsToTheNearestMultiple_AndIgnoresABadStep)
{
	BOOST_TEST(snap_to_step(0.26f, 0.1f) == 0.3f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(snap_to_step(-0.26f, 0.1f) == -0.3f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(snap_to_step(0.26f, 0.f) == 0.26f);
	BOOST_TEST(snap_to_step(0.26f, -1.f) == 0.26f);
	BOOST_TEST(snap_to_step(0.26f, std::numeric_limits<float>::quiet_NaN()) == 0.26f);
	BOOST_TEST(near(snap_to_step(glm::vec3(0.26f, 0.04f, 1.f), 0.1f), glm::vec3(0.3f, 0.f, 1.f)));
}

BOOST_AUTO_TEST_CASE(MovesBetween_OnlyMovedVertices_ListsThemInOrder)
{
	poly_mesh moved = mesh;
	const vertex_id first = moved.vertices().begin()->first;
	moved.set_position(first, glm::vec3(5.f));
	const auto moves = moves_between(mesh, moved);
	BOOST_REQUIRE(moves.has_value());
	BOOST_REQUIRE(moves->size() == 1u);
	BOOST_TEST((*moves)[0].id == first);
	BOOST_TEST(near((*moves)[0].after, glm::vec3(5.f)));
	BOOST_TEST(moves_between(mesh, mesh).value().empty());
}

BOOST_AUTO_TEST_CASE(MovesBetween_ATopologyChange_IsNothing)
{
	poly_mesh changed = mesh;
	changed.remove_face(changed.faces().begin()->first);
	BOOST_TEST(!moves_between(mesh, changed).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
