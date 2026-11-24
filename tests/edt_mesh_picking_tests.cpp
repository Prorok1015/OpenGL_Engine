#include <boost/test/unit_test.hpp>
#include "edt_mesh_adjacency.h"
#include "edt_mesh_bake.h"
#include "edt_mesh_picking.h"
#include "edt_mesh_selection.h"
#include "edt_mesh_shapes.h"
#include <cmath>
#include <optional>
#include <utility>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

namespace {

using namespace edt::mesh;

constexpr float k_image = 200.f;

screen_view camera_at(const glm::vec3& eye, const glm::vec3& target)
{
	const glm::mat4 view_matrix = glm::lookAt(eye, target, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(45.f), 1.f, 0.1f, 100.f);
	return screen_view(projection * view_matrix, { 0.f, 0.f }, { k_image, k_image });
}

struct scene
{
	explicit scene(poly_mesh built) : mesh(std::move(built)), adjacency(mesh), picker(mesh) {}

	pick_query query(const screen_view& view, bool xray = false) const { return pick_query{ mesh, adjacency, picker, view, xray }; }

	poly_mesh mesh;
	mesh_adjacency adjacency;
	mesh_picker picker;
};

struct picking_fixture
{
	picking_fixture()
	{
		shape_result result = make_shape(box_params{});
		BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
		mesh = std::move(*result.mesh);
		adjacency.emplace(mesh);
		picker.emplace(mesh);
		view = camera_at({ 0.f, 0.f, 3.f }, { 0.f, 0.f, 0.f });
	}

	pick_query query(bool xray = false) const { return pick_query{ mesh, *adjacency, *picker, view, xray }; }
	pick_query query_with(const screen_view& other, bool xray = false) const { return pick_query{ mesh, *adjacency, *picker, other, xray }; }

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

BOOST_AUTO_TEST_CASE(AnOffsetNonSquareImage_FlipsYAndScalesEachAxis)
{
	const screen_view offset(glm::mat4(1.f), { 50.f, 30.f }, { 300.f, 150.f });
	const std::optional<glm::vec2> pixel = offset.project({ 0.f, 0.5f, 0.f });
	BOOST_REQUIRE(pixel.has_value());
	BOOST_TEST(pixel->x == 200.f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(pixel->y == 67.5f, boost::test_tools::tolerance(1e-4f));

	const std::optional<ds::ray3> ray = offset.ray_through(*pixel);
	BOOST_REQUIRE(ray.has_value());
	BOOST_TEST(std::abs(ray->origin.x) < 1e-4f);
	BOOST_TEST(ray->origin.y == 0.5f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(ray->direction.z == 1.f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(TriangulateFace_GivesTheTrianglesTheBakeCuts_AndNothingForAFaceItLeavesOut)
{
	poly_mesh mixed;
	const auto add = [&mixed](std::vector<glm::vec3> points) {
		std::vector<corner> corners;
		for (const glm::vec3& p : points) corners.push_back(corner{ mixed.add_vertex(p) });
		return mixed.add_face(std::move(corners));
	};
	const face_id quad = add({ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f } });
	const face_id concave = add({ { 0.f, 0.f, 1.f }, { 2.f, 0.f, 1.f }, { 2.f, 1.f, 1.f }, { 1.f, 1.f, 1.f }, { 1.f, 2.f, 1.f }, { 0.f, 2.f, 1.f } });
	const face_id degenerate = add({ { 0.f, 0.f, 2.f }, { 1.f, 0.f, 2.f }, { 2.f, 0.f, 2.f } });

	BOOST_REQUIRE(triangulate_face(mixed, quad).has_value());
	BOOST_TEST(triangulate_face(mixed, quad)->size() == 2u);
	BOOST_REQUIRE(triangulate_face(mixed, concave).has_value());
	BOOST_TEST(triangulate_face(mixed, concave)->size() == 4u);
	BOOST_TEST(!triangulate_face(mixed, degenerate).has_value());
	BOOST_TEST(!triangulate_face(mixed, 9999u).has_value());

	const baked_mesh baked = bake(mixed);
	std::size_t triangles = 0;
	std::vector<face_id> left_out;
	for (const auto& [id, shape] : mixed.faces()) {
		const auto cut = triangulate_face(mixed, id);
		if (cut) {
			triangles += cut->size();
		} else {
			left_out.push_back(id);
		}
	}
	BOOST_TEST(triangles == baked.geometry.indices.size() / 3);
	BOOST_TEST(left_out == baked.skipped_faces, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(AFacePartlyCovered_IsInABoxWhenItsOwnPointIsSeen_AndNotWhenThatPointIsCovered)
{
	const auto two_quads = [](float cover) {
		poly_mesh planes;
		const auto quad = [&planes](float half, float z) {
			const vertex_id a = planes.add_vertex({ -half, -half, z });
			const vertex_id b = planes.add_vertex({ half, -half, z });
			const vertex_id c = planes.add_vertex({ half, half, z });
			const vertex_id d = planes.add_vertex({ -half, half, z });
			planes.add_face({ { a }, { b }, { c }, { d } });
		};
		quad(1.f, -1.f);
		quad(cover, 0.5f);
		return planes;
	};
	const screen_view view = camera_at({ 0.f, 0.f, 3.f }, { 0.f, 0.f, 0.f });

	const scene small_cover(two_quads(0.1f));
	mesh_selection selection;
	selection.set_mode(select_mode::face, small_cover.mesh, small_cover.adjacency);
	box_select(selection, small_cover.query(view), whole_image, select_op::replace);
	BOOST_TEST(selection.faces().size() == 2u);

	const scene big_cover(two_quads(0.5f));
	selection = mesh_selection();
	selection.set_mode(select_mode::face, big_cover.mesh, big_cover.adjacency);
	box_select(selection, big_cover.query(view), whole_image, select_op::replace);
	BOOST_TEST(selection.faces().size() == 1u);
}

BOOST_AUTO_TEST_CASE(ABoxGivenFromTheOtherCorner_IsTheSameBox)
{
	const pixel_rect reversed{ { k_image, k_image }, { 0.f, 0.f } };
	box_select(selection, query(), reversed, select_op::replace);
	BOOST_TEST(selection.vertices().size() == 4u);
}

BOOST_AUTO_TEST_CASE(AnEdgeAcrossTheCameraPlane_IsPickedByItsPartInFront_AndItsFarEndIsInNoBox)
{
	poly_mesh across;
	const vertex_id a = across.add_vertex({ -1.f, 0.f, 0.f });
	const vertex_id b = across.add_vertex({ -1.f, 0.f, 10.f });
	const vertex_id c = across.add_vertex({ -1.f, 1.f, 0.f });
	across.add_face({ { a }, { b }, { c } });
	const scene crossing(std::move(across));
	const screen_view view = camera_at({ 0.f, 0.f, 3.f }, { 0.f, 0.f, 0.f });
	BOOST_REQUIRE(view.depth({ -1.f, 0.f, 10.f }) < 0.f);

	const std::optional<edge_key> found = pick_edge(crossing.query(view), { 10.f, 100.f }, 2.f);
	BOOST_REQUIRE(found.has_value());
	BOOST_TEST((*found == edge_key::of(a, b)));

	mesh_selection selection;
	box_select(selection, crossing.query(view, true), whole_image, select_op::replace);
	BOOST_TEST(selection.vertices().size() == 2u);
	BOOST_TEST(!selection.vertices().contains(b));

	selection.set_mode(select_mode::edge, crossing.mesh, crossing.adjacency);
	box_select(selection, crossing.query(view, true), whole_image, select_op::replace);
	BOOST_TEST(selection.edges().size() == 1u);
	BOOST_TEST(selection.edges().contains(edge_key::of(a, c)));
}

BOOST_AUTO_TEST_CASE(ACameraInThePlaneOfASide_StillPicksItsSilhouetteCornerAndTheFaceInFront)
{
	const screen_view grazing = camera_at({ 0.5f, 0.f, 3.f }, { 0.5f, 0.f, 0.f });
	const vertex_id corner = vertex_at({ 0.5f, 0.5f, 0.5f });
	const std::optional<glm::vec2> pixel = grazing.project({ 0.5f, 0.5f, 0.5f });
	BOOST_REQUIRE(pixel.has_value());
	BOOST_TEST((pick_vertex(query_with(grazing), *pixel, 3.f) == std::optional<vertex_id>(corner)));

	const std::optional<glm::vec2> beside_edge = grazing.project({ 0.49f, 0.f, 0.5f });
	BOOST_REQUIRE(beside_edge.has_value());
	const std::optional<face_id> face_found = pick_face(query_with(grazing), *beside_edge);
	BOOST_REQUIRE(face_found.has_value());
	BOOST_TEST(face_z(*face_found) == 0.5f, boost::test_tools::tolerance(1e-4f));
}

BOOST_AUTO_TEST_CASE(AfterAnEdit_ARebuiltPickerAndAdjacency_FindTheMovedVertexAndNotTheOldPlace)
{
	const vertex_id corner = vertex_at({ 0.5f, 0.5f, 0.5f });
	const glm::vec2 before = pixel_of({ 0.5f, 0.5f, 0.5f });
	BOOST_REQUIRE(mesh.set_position(corner, { 0.5f, 0.3f, 0.5f }));
	BOOST_TEST(!picker->is_current(mesh));
	BOOST_TEST(!adjacency->is_current(mesh));

	adjacency.emplace(mesh);
	picker.emplace(mesh);
	BOOST_TEST(picker->is_current(mesh));
	BOOST_TEST((pick_vertex(query(), pixel_of({ 0.5f, 0.3f, 0.5f }), 2.f) == std::optional<vertex_id>(corner)));
	BOOST_TEST(!pick_vertex(query(), before, 2.f).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
