#include <boost/test/unit_test.hpp>
#include "edt_mesh_adjacency.h"
#include "edt_mesh_selection.h"
#include "edt_mesh_shapes.h"
#include <iterator>
#include <optional>

namespace {

using namespace edt::mesh;

struct cube_fixture
{
	cube_fixture()
	{
		shape_result result = make_shape(box_params{});
		BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
		mesh = std::move(*result.mesh);
		adjacency.emplace(mesh);
	}

	poly_mesh mesh;
	std::optional<mesh_adjacency> adjacency;
	mesh_selection selection;

	face_id first_face() const { return mesh.faces().begin()->first; }
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshSelectionTests, cube_fixture)

BOOST_AUTO_TEST_CASE(ANewSelection_IsEmpty_InVertexMode)
{
	BOOST_TEST(selection.empty());
	BOOST_TEST(selection.mode() == select_mode::vertex);
}

BOOST_AUTO_TEST_CASE(Replace_KeepsOnlyTheNewElement_AddKeepsBoth_ToggleRemovesAPresentOne)
{
	const vertex_id first = mesh.vertices().begin()->first;
	const vertex_id second = std::next(mesh.vertices().begin())->first;

	selection.apply_vertex(first, select_op::replace);
	selection.apply_vertex(second, select_op::add);
	BOOST_TEST(selection.vertices().size() == 2u);

	selection.apply_vertex(first, select_op::toggle);
	BOOST_TEST(selection.vertices().size() == 1u);
	BOOST_TEST(selection.vertices().contains(second));

	selection.apply_vertex(first, select_op::replace);
	BOOST_TEST(selection.vertices().size() == 1u);
	BOOST_TEST(selection.vertices().contains(first));
}

BOOST_AUTO_TEST_CASE(AnElementOfAnotherMode_IsIgnored)
{
	selection.apply_face(first_face(), select_op::replace);
	selection.apply_edge(adjacency->edges().front(), select_op::replace);
	BOOST_TEST(selection.empty());
}

BOOST_AUTO_TEST_CASE(SelectAll_CountsTheElementsOfTheMode)
{
	selection.select_all(mesh, *adjacency);
	BOOST_TEST(selection.vertices().size() == 8u);

	selection.set_mode(select_mode::edge, mesh, *adjacency);
	selection.select_all(mesh, *adjacency);
	BOOST_TEST(selection.edges().size() == 12u);

	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.select_all(mesh, *adjacency);
	BOOST_TEST(selection.faces().size() == 6u);
}

BOOST_AUTO_TEST_CASE(Invert_SelectsTheRest_AndTwiceGivesTheOriginal)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(first_face(), select_op::replace);

	selection.invert(mesh, *adjacency);
	BOOST_TEST(selection.faces().size() == 5u);
	BOOST_TEST(!selection.faces().contains(first_face()));

	selection.invert(mesh, *adjacency);
	BOOST_TEST(selection.faces().size() == 1u);
	BOOST_TEST(selection.faces().contains(first_face()));
}

BOOST_AUTO_TEST_CASE(FaceToVertexMode_KeepsTheFourCorners_AndBackToFaceTheFace)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(first_face(), select_op::replace);

	selection.set_mode(select_mode::vertex, mesh, *adjacency);
	BOOST_TEST(selection.vertices().size() == 4u);
	BOOST_TEST(selection.faces().empty());

	selection.set_mode(select_mode::face, mesh, *adjacency);
	BOOST_TEST(selection.faces().size() == 1u);
	BOOST_TEST(selection.faces().contains(first_face()));
}

BOOST_AUTO_TEST_CASE(TwoVerticesOfAnEdge_GiveThatEdge_OneVertexGivesNone)
{
	const edge_key edge = adjacency->edges().front();
	selection.apply_vertex(edge.a, select_op::replace);

	selection.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_TEST(selection.empty());

	selection.set_mode(select_mode::vertex, mesh, *adjacency);
	selection.apply_vertex(edge.a, select_op::replace);
	selection.apply_vertex(edge.b, select_op::add);
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_TEST(selection.edges().contains(edge));
}

BOOST_AUTO_TEST_CASE(EdgesAroundAFace_GiveThatFace_ButThreeOfFourDoNot)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(first_face(), select_op::replace);
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_REQUIRE_EQUAL(selection.edges().size(), 4u);

	mesh_selection three = selection;
	three.apply_edge(*selection.edges().begin(), select_op::toggle);
	three.set_mode(select_mode::face, mesh, *adjacency);
	BOOST_TEST(three.faces().empty());

	selection.set_mode(select_mode::face, mesh, *adjacency);
	BOOST_TEST(selection.faces().contains(first_face()));
}

BOOST_AUTO_TEST_CASE(TouchedVertices_OfAFace_AreItsCorners)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(first_face(), select_op::replace);
	BOOST_TEST(selection.touched_vertices(mesh).size() == 4u);
}

BOOST_AUTO_TEST_CASE(Prune_DropsAFaceTheMeshNoLongerHas)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(first_face(), select_op::replace);
	selection.apply_face(std::next(mesh.faces().begin())->first, select_op::add);

	const face_id removed = first_face();
	BOOST_REQUIRE(mesh.remove_face(removed));
	const mesh_adjacency after(mesh);
	selection.prune(mesh, after);

	BOOST_TEST(selection.faces().size() == 1u);
	BOOST_TEST(!selection.faces().contains(removed));
}

BOOST_AUTO_TEST_SUITE_END()
