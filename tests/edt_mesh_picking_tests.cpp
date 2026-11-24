#include <boost/test/unit_test.hpp>
#include "edt_mesh_adjacency.h"
#include "edt_mesh_picking.h"
#include "edt_mesh_selection.h"
#include "edt_mesh_shapes.h"
#include <cmath>
#include <optional>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>

namespace {

using namespace edt::mesh;

constexpr float k_image = 200.f;

struct picking_fixture
{
	picking_fixture()
	{
		shape_result result = make_shape(box_params{});
		BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
		mesh = std::move(*result.mesh);
		adjacency.emplace(mesh);
		picker.emplace(mesh);
		const glm::mat4 view_matrix = glm::lookAt(glm::vec3{ 0.f, 0.f, 3.f }, glm::vec3{ 0.f }, glm::vec3{ 0.f, 1.f, 0.f });
		const glm::mat4 projection = glm::perspective(glm::radians(45.f), 1.f, 0.1f, 100.f);
		view.view_projection = projection * view_matrix;
		view.origin = { 0.f, 0.f };
		view.size = { k_image, k_image };
	}

	pick_query query(bool xray = false) const { return pick_query{ mesh, *adjacency, *picker, view, xray }; }

	vertex_id vertex_at(const glm::vec3& position) const
	{
		for (const auto& [id, v] : mesh.vertices()) {
			if (glm::distance(v.position, position) < 1e-4f) return id;
		}
		BOOST_FAIL("no vertex at the position");
		return invalid_id;
	}

	glm::vec2 pixel_of(const glm::vec3& position) const
	{
		const std::optional<glm::vec2> pixel = view.project(position);
		BOOST_REQUIRE(pixel.has_value());
		return *pixel;
	}

	float face_z(face_id id) const
	{
		const face* shape = mesh.find_face(id);
		BOOST_REQUIRE(shape != nullptr);
		float sum = 0.f;
		for (const corner& c : shape->corners) sum += mesh.find_vertex(c.vertex)->position.z;
		return sum / static_cast<float>(shape->corners.size());
	}

	poly_mesh mesh;
	std::optional<mesh_adjacency> adjacency;
	std::optional<mesh_picker> picker;
	screen_view view;
	mesh_selection selection;
};

const pixel_rect whole_image{ { 0.f, 0.f }, { k_image, k_image } };

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshPickingTests, picking_fixture)

BOOST_AUTO_TEST_CASE(TheCentreOfTheView_ProjectsToTheCentreOfTheImage_AndTheRayGoesStraightIn)
{
	const glm::vec2 centre = pixel_of({ 0.f, 0.f, 0.f });
	BOOST_TEST(centre.x == 100.f, boost::test_tools::tolerance(1e-3f));
	BOOST_TEST(centre.y == 100.f, boost::test_tools::tolerance(1e-3f));

	const std::optional<ds::ray3> ray = view.ray_through(centre);
	BOOST_REQUIRE(ray.has_value());
	BOOST_TEST(ray->direction.z == -1.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(std::abs(ray->direction.x) < 1e-4f);
}

BOOST_AUTO_TEST_CASE(APointBehindTheCamera_HasNoPixel)
{
	BOOST_TEST(!view.project({ 0.f, 0.f, 10.f }).has_value());
}

BOOST_AUTO_TEST_CASE(TheRayThroughAProjectedPoint_PassesThatPoint)
{
	const glm::vec3 point{ 0.3f, -0.2f, 0.1f };
	const std::optional<ds::ray3> ray = view.ray_through(pixel_of(point));
	BOOST_REQUIRE(ray.has_value());
	const float along = glm::dot(point - ray->origin, ray->direction);
	BOOST_TEST(glm::distance(ray->at(along), point) < 1e-3f);
}

BOOST_AUTO_TEST_CASE(ACubeIsTwelveTriangles_AndTheCacheIsStaleAfterAChange)
{
	BOOST_TEST(picker->triangle_count() == 12u);
	BOOST_TEST(picker->is_current(mesh));
	BOOST_REQUIRE(mesh.set_position(vertex_at({ 0.5f, 0.5f, 0.5f }), { 0.6f, 0.5f, 0.5f }));
	BOOST_TEST(!picker->is_current(mesh));
}

BOOST_AUTO_TEST_CASE(ARayAtTheCube_HitsTheNearFaceAtTheRightDistance)
{
	const std::optional<face_hit> hit = picker->first_hit(ds::ray3({ 0.f, 0.f, 3.f }, { 0.f, 0.f, -1.f }));
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->distance == 2.5f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(!hit->back_face);
	BOOST_TEST(face_z(hit->face) == 0.5f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(ARayFromBehind_HitsTheFarFace_AndACullingOneOnlyTheFrontOnes)
{
	const ds::ray3 from_inside({ 0.f, 0.f, 0.f }, { 0.f, 0.f, -1.f });
	const std::optional<face_hit> hit = picker->first_hit(from_inside);
	BOOST_REQUIRE(hit.has_value());
	BOOST_TEST(hit->back_face);
	BOOST_TEST(!picker->first_hit(from_inside, true).has_value());
}

BOOST_AUTO_TEST_CASE(PickFace_TakesTheFaceUnderThePixel_AndNothingOutsideTheCube)
{
	const std::optional<face_id> centre = pick_face(query(), { 100.f, 100.f });
	BOOST_REQUIRE(centre.has_value());
	BOOST_TEST(face_z(*centre) == 0.5f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(!pick_face(query(), { 5.f, 5.f }).has_value());
}

BOOST_AUTO_TEST_CASE(PickVertex_TakesTheNearestWithinTheRadius)
{
	const vertex_id corner = vertex_at({ 0.5f, 0.5f, 0.5f });
	const glm::vec2 pixel = pixel_of({ 0.5f, 0.5f, 0.5f });

	BOOST_TEST((pick_vertex(query(), pixel + glm::vec2{ 2.f, 2.f }, 10.f) == std::optional<vertex_id>(corner)));
	BOOST_TEST(!pick_vertex(query(), pixel + glm::vec2{ 30.f, 30.f }, 10.f).has_value());
}

BOOST_AUTO_TEST_CASE(PickVertex_SkipsAVertexBehindTheNearFace_UnlessXray)
{
	const glm::vec3 hidden{ 0.5f, 0.5f, -0.5f };
	const glm::vec2 pixel = pixel_of(hidden);

	BOOST_TEST(!pick_vertex(query(), pixel, 0.5f).has_value());
	BOOST_TEST((pick_vertex(query(true), pixel, 0.5f) == std::optional<vertex_id>(vertex_at(hidden))));
}

BOOST_AUTO_TEST_CASE(PickEdge_TakesTheEdgeNearTheCursor_AndNotOneBehindTheNearFace)
{
	const glm::vec3 top_front{ 0.f, 0.5f, 0.5f };
	const std::optional<edge_key> found = pick_edge(query(), pixel_of(top_front), 3.f);
	BOOST_REQUIRE(found.has_value());
	BOOST_TEST((*found == edge_key::of(vertex_at({ -0.5f, 0.5f, 0.5f }), vertex_at({ 0.5f, 0.5f, 0.5f }))));

	const glm::vec3 top_back{ 0.f, 0.5f, -0.5f };
	BOOST_TEST(!pick_edge(query(), pixel_of(top_back), 1.f).has_value());
	BOOST_TEST(pick_edge(query(true), pixel_of(top_back), 1.f).has_value());
}

BOOST_AUTO_TEST_CASE(AClickInFaceMode_SelectsTheFaceUnderIt_AndAMissClearsOnlyForReplace)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);

	BOOST_TEST(click_select(selection, query(), { 100.f, 100.f }, 8.f, select_op::replace));
	BOOST_TEST(selection.faces().size() == 1u);

	BOOST_TEST(!click_select(selection, query(), { 5.f, 5.f }, 8.f, select_op::add));
	BOOST_TEST(selection.faces().size() == 1u);

	BOOST_TEST(!click_select(selection, query(), { 5.f, 5.f }, 8.f, select_op::replace));
	BOOST_TEST(selection.empty());
}

BOOST_AUTO_TEST_CASE(ShiftClick_ToggleTakesAVertexOutAgain)
{
	const glm::vec2 pixel = pixel_of({ 0.5f, 0.5f, 0.5f });
	BOOST_TEST(click_select(selection, query(), pixel, 8.f, select_op::replace));
	BOOST_TEST(selection.vertices().size() == 1u);
	BOOST_TEST(click_select(selection, query(), pixel, 8.f, select_op::toggle));
	BOOST_TEST(selection.empty());
}

BOOST_AUTO_TEST_CASE(ABoxInVertexMode_TakesTheFourVisibleCorners_XrayAllEight)
{
	box_select(selection, query(), whole_image, select_op::replace);
	BOOST_TEST(selection.vertices().size() == 4u);

	box_select(selection, query(true), whole_image, select_op::replace);
	BOOST_TEST(selection.vertices().size() == 8u);
}

BOOST_AUTO_TEST_CASE(ABoxInFaceMode_TakesTheNearFaceOnly_XrayAllSix)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);

	box_select(selection, query(), whole_image, select_op::replace);
	BOOST_TEST(selection.faces().size() == 1u);

	box_select(selection, query(true), whole_image, select_op::replace);
	BOOST_TEST(selection.faces().size() == 6u);
}

BOOST_AUTO_TEST_CASE(ABoxInEdgeMode_NeedsBothEndsInIt)
{
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	box_select(selection, query(true), whole_image, select_op::replace);
	BOOST_TEST(selection.edges().size() == 12u);

	const glm::vec2 top_left = pixel_of({ -0.5f, 0.5f, 0.5f });
	const pixel_rect corner_only{ top_left - glm::vec2{ 4.f }, top_left + glm::vec2{ 4.f } };
	box_select(selection, query(true), corner_only, select_op::replace);
	BOOST_TEST(selection.empty());
}

BOOST_AUTO_TEST_CASE(ABoxWithAdd_KeepsWhatWasThere_WithToggleTakesItOut)
{
	const glm::vec2 corner = pixel_of({ 0.5f, 0.5f, 0.5f });
	click_select(selection, query(), corner, 8.f, select_op::replace);
	BOOST_REQUIRE(selection.vertices().size() == 1u);

	box_select(selection, query(), whole_image, select_op::add);
	BOOST_TEST(selection.vertices().size() == 4u);

	box_select(selection, query(), whole_image, select_op::toggle);
	BOOST_TEST(selection.empty());
}

BOOST_AUTO_TEST_SUITE_END()
