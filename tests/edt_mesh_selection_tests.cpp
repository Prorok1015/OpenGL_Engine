#include <boost/test/unit_test.hpp>
#include "edt_mesh_adjacency.h"
#include "edt_mesh_selection.h"
#include "edt_mesh_shapes.h"
#include <iterator>
#include <optional>
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
	BOOST_TEST((selection.mode() == select_mode::vertex));
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

BOOST_AUTO_TEST_CASE(AnEmptyMesh_HasNothingToSelect_AndAnEmptySelectionSurvivesEveryOperation)
{
	const poly_mesh empty;
	const mesh_adjacency adjacency_of_empty(empty);
	mesh_selection none;

	none.select_all(empty, adjacency_of_empty);
	BOOST_TEST(none.empty());
	none.invert(empty, adjacency_of_empty);
	BOOST_TEST(none.empty());
	none.set_mode(select_mode::face, empty, adjacency_of_empty);
	none.prune(empty, adjacency_of_empty);
	BOOST_TEST(none.empty());
	BOOST_TEST(none.touched_vertices(empty).empty());
}

BOOST_AUTO_TEST_CASE(ASingleQuad_HasFourBoundaryEdges_AndItsEdgesGiveBackTheFace)
{
	poly_mesh quad;
	const vertex_id a = quad.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = quad.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = quad.add_vertex({ 1.f, 1.f, 0.f });
	const vertex_id d = quad.add_vertex({ 0.f, 1.f, 0.f });
	const face_id id = quad.add_face({ { a }, { b }, { c }, { d } });
	const mesh_adjacency adjacency_of_quad(quad);

	mesh_selection picked;
	picked.set_mode(select_mode::edge, quad, adjacency_of_quad);
	picked.select_all(quad, adjacency_of_quad);
	BOOST_TEST(picked.edges().size() == 4u);

	picked.set_mode(select_mode::face, quad, adjacency_of_quad);
	BOOST_TEST(picked.faces().size() == 1u);
	BOOST_TEST(picked.faces().contains(id));
}

BOOST_AUTO_TEST_CASE(ANonManifoldEdgeSharedByThreeFaces_GivesTheThreeFaces_WhenAllTheirEdgesAreSelected)
{
	poly_mesh fan;
	const vertex_id a = fan.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = fan.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = fan.add_vertex({ 0.f, 1.f, 0.f });
	const vertex_id d = fan.add_vertex({ 0.f, -1.f, 0.f });
	const vertex_id e = fan.add_vertex({ 0.f, 0.f, 1.f });
	fan.add_face({ { a }, { b }, { c } });
	fan.add_face({ { a }, { b }, { d } });
	fan.add_face({ { a }, { b }, { e } });
	const mesh_adjacency adjacency_of_fan(fan);
	BOOST_REQUIRE(adjacency_of_fan.is_non_manifold(edge_key::of(a, b)));

	mesh_selection picked;
	picked.set_mode(select_mode::edge, fan, adjacency_of_fan);
	picked.select_all(fan, adjacency_of_fan);
	BOOST_TEST(picked.edges().size() == 7u);

	picked.set_mode(select_mode::face, fan, adjacency_of_fan);
	BOOST_TEST(picked.faces().size() == 3u);
}

BOOST_AUTO_TEST_CASE(TheSameElementAddedTwice_IsSelectedOnce)
{
	const vertex_id first = mesh.vertices().begin()->first;
	selection.apply_vertex(first, select_op::add);
	selection.apply_vertex(first, select_op::add);
	BOOST_TEST(selection.vertices().size() == 1u);
}

BOOST_AUTO_TEST_CASE(Prune_DropsVerticesAndEdgesNoFaceUsesAnymore_AfterRemoveVertex)
{
	poly_mesh two;
	const vertex_id a = two.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = two.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = two.add_vertex({ 0.f, 1.f, 0.f });
	const vertex_id d = two.add_vertex({ 1.f, 1.f, 0.f });
	const face_id first = two.add_face({ { a }, { b }, { c } });
	two.add_face({ { b }, { d }, { c } });

	mesh_selection picked;
	picked.apply_vertex(a, select_op::add);
	picked.apply_vertex(d, select_op::add);
	picked.set_mode(select_mode::edge, two, mesh_adjacency(two));
	picked.apply_edge(edge_key::of(a, b), select_op::add);
	picked.apply_edge(edge_key::of(b, d), select_op::add);
	BOOST_REQUIRE_EQUAL(picked.edges().size(), 2u);

	BOOST_REQUIRE(two.remove_face(first));
	BOOST_REQUIRE(two.remove_vertex(a));
	const mesh_adjacency after(two);
	picked.prune(two, after);
	BOOST_TEST(picked.edges().size() == 1u);
	BOOST_TEST(picked.edges().contains(edge_key::of(b, d)));

	mesh_selection by_vertex;
	by_vertex.apply_vertex(a, select_op::add);
	by_vertex.apply_vertex(d, select_op::add);
	by_vertex.prune(two, after);
	BOOST_TEST(by_vertex.vertices().size() == 1u);
	BOOST_TEST(by_vertex.vertices().contains(d));
}

BOOST_AUTO_TEST_CASE(VertexToFace_NeedsEveryCornerOfTheFace)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(first_face(), select_op::replace);
	selection.set_mode(select_mode::vertex, mesh, *adjacency);
	BOOST_REQUIRE_EQUAL(selection.vertices().size(), 4u);

	selection.apply_vertex(*selection.vertices().begin(), select_op::toggle);
	selection.set_mode(select_mode::face, mesh, *adjacency);
	BOOST_TEST(selection.faces().empty());
}

BOOST_AUTO_TEST_CASE(SettingTheModeItAlreadyHas_ChangesNothing)
{
	selection.apply_vertex(mesh.vertices().begin()->first, select_op::replace);
	selection.set_mode(select_mode::vertex, mesh, *adjacency);
	BOOST_TEST(selection.vertices().size() == 1u);
}

BOOST_AUTO_TEST_CASE(FaceToEdge_GivesExactlyTheTwelveEdgesOfAWholeCube)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.select_all(mesh, *adjacency);
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_TEST(selection.edges().size() == 12u);

	mesh_selection one;
	one.set_mode(select_mode::face, mesh, *adjacency);
	one.apply_face(first_face(), select_op::replace);
	one.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_TEST(one.edges().size() == 4u);
}

BOOST_AUTO_TEST_SUITE_END()
