#include <boost/test/unit_test.hpp>
#include "edt_mesh_shapes.h"
#include "edt_mesh_view.h"
#include <optional>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>

namespace {

using namespace edt::mesh;

struct view_fixture
{
	view_fixture()
	{
		shape_result result = make_shape(box_params{});
		BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
		mesh = std::move(*result.mesh);
	}

	face_id first_face() const { return mesh.faces().begin()->first; }

	poly_mesh mesh;
	mesh_view view;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshViewTests, view_fixture)

BOOST_AUTO_TEST_CASE(ANewView_IsInVertexMode_NoXray_NothingSelectedOrHovered)
{
	BOOST_TEST((view.mode() == select_mode::vertex));
	BOOST_TEST(!view.xray());
	BOOST_TEST(view.selection().empty());
	BOOST_TEST(view.hover().empty());
}

BOOST_AUTO_TEST_CASE(TheCaches_FollowTheMeshGeneration)
{
	BOOST_TEST(view.adjacency(mesh).is_current(mesh));
	BOOST_TEST(view.picker(mesh).is_current(mesh));
	BOOST_REQUIRE(mesh.set_position(mesh.vertices().begin()->first, { 0.9f, 0.9f, 0.9f }));
	BOOST_TEST(view.adjacency(mesh).is_current(mesh));
	BOOST_TEST(view.picker(mesh).is_current(mesh));
}

BOOST_AUTO_TEST_CASE(SetMode_CarriesTheSelectionOver)
{
	view.adjacency(mesh);
	view.selection().select_all(mesh, view.adjacency(mesh));
	BOOST_REQUIRE_EQUAL(view.selection().vertices().size(), 8u);

	view.set_mode(select_mode::face, mesh);
	BOOST_TEST((view.mode() == select_mode::face));
	BOOST_TEST(view.selection().faces().size() == 6u);
}

BOOST_AUTO_TEST_CASE(AnEdit_DropsWhatTheMeshLost_FromTheSelection_AndClearsTheHover)
{
	view.set_mode(select_mode::face, mesh);
	view.selection().apply_face(first_face(), select_op::replace);
	view.set_hover({ std::nullopt, std::nullopt, first_face() });

	BOOST_REQUIRE(mesh.remove_face(first_face()));
	view.adjacency(mesh);
	BOOST_TEST(view.selection().empty());
	BOOST_TEST(view.hover().empty());
}

BOOST_AUTO_TEST_CASE(Reset_ForgetsTheIds_ButKeepsTheModeAndXray)
{
	view.set_mode(select_mode::face, mesh);
	view.selection().apply_face(first_face(), select_op::replace);
	view.set_hover({ std::nullopt, std::nullopt, first_face() });
	view.set_xray(true);
	view.reset();

	BOOST_TEST(view.selection().empty());
	BOOST_TEST(view.hover().empty());
	BOOST_TEST((view.mode() == select_mode::face));
	BOOST_TEST(view.xray());
}

BOOST_AUTO_TEST_CASE(TheCaches_AreRebuiltForAChangedMesh_NotJustMarkedStale)
{
	poly_mesh grown;
	const vertex_id a = grown.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = grown.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = grown.add_vertex({ 0.f, 1.f, 0.f });
	const vertex_id d = grown.add_vertex({ 1.f, 1.f, 0.f });
	grown.add_face({ { a }, { b }, { c } });
	BOOST_TEST(view.adjacency(grown).edges().size() == 3u);
	BOOST_TEST(view.picker(grown).triangle_count() == 1u);

	grown.add_face({ { b }, { d }, { c } });
	BOOST_TEST(view.adjacency(grown).edges().size() == 5u);
	BOOST_TEST(view.picker(grown).triangle_count() == 2u);
}

BOOST_AUTO_TEST_CASE(CurrentSelection_DropsWhatAnEditRemoved_BeforeItIsRead)
{
	view.set_mode(select_mode::face, mesh);
	view.selection().apply_face(first_face(), select_op::replace);
	BOOST_REQUIRE(mesh.remove_face(first_face()));

	BOOST_TEST(view.current_selection(mesh).empty());
}

BOOST_AUTO_TEST_CASE(Query_CarriesTheXrayFlag_AndPicksOnTheCurrentCaches)
{
	const glm::mat4 look = glm::lookAt(glm::vec3{ 0.f, 0.f, 3.f }, glm::vec3{ 0.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const screen_view camera(glm::perspective(glm::radians(45.f), 1.f, 0.1f, 100.f) * look, { 0.f, 0.f }, { 200.f, 200.f });

	BOOST_TEST(!view.query(mesh, camera).xray);
	view.set_xray(true);
	BOOST_TEST(view.query(mesh, camera).xray);
	BOOST_TEST(pick_face(view.query(mesh, camera), { 100.f, 100.f }).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
