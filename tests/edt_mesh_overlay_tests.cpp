#include <boost/test/unit_test.hpp>
#include "edt_mesh_adjacency.h"
#include "edt_mesh_overlay.h"
#include "edt_mesh_picking.h"
#include "edt_mesh_selection.h"
#include "edt_mesh_shapes.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>

namespace {

using namespace edt::mesh;

constexpr float k_image = 200.f;

screen_view front_camera()
{
	const glm::mat4 view_matrix = glm::lookAt(glm::vec3{ 0.f, 0.f, 3.f }, glm::vec3{ 0.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const glm::mat4 projection = glm::perspective(glm::radians(45.f), 1.f, 0.1f, 100.f);
	return screen_view(projection * view_matrix, { 0.f, 0.f }, { k_image, k_image });
}

struct overlay_fixture
{
	overlay_fixture()
	{
		shape_result result = make_shape(box_params{});
		BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
		mesh = std::move(*result.mesh);
		adjacency.emplace(mesh);
		view = front_camera();
	}

	overlay_frame build(bool xray = false, std::size_t budget = k_overlay_segment_budget, std::optional<glm::vec2> cursor = std::nullopt) const
	{
		overlay_options options;
		options.xray = xray;
		options.segment_budget = budget;
		options.cursor = cursor;
		return build_overlay(mesh, *adjacency, selection, hover, view, options);
	}

	face_id front_face() const
	{
		for (const auto& [id, f] : mesh.faces()) {
			float z = 0.f;
			for (const corner& c : f.corners) z += mesh.find_vertex(c.vertex)->position.z;
			if (z / static_cast<float>(f.corners.size()) > 0.4f) return id;
		}
		BOOST_FAIL("no face at the front");
		return invalid_id;
	}

	static std::size_t count(const overlay_frame& frame, overlay_kind kind)
	{
		return static_cast<std::size_t>(std::ranges::count_if(frame.segments, [kind](const overlay_segment& s) { return s.kind == kind; }));
	}

	poly_mesh mesh;
	std::optional<mesh_adjacency> adjacency;
	screen_view view;
	mesh_selection selection;
	hover_target hover;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshOverlayTests, overlay_fixture)

BOOST_AUTO_TEST_CASE(WithoutXray_OnlyTheEdgesOfTheFaceLookingAtTheCameraAreDrawn)
{
	const overlay_frame frame = build();
	BOOST_TEST(frame.segments.size() == 4u);
	BOOST_TEST(frame.omitted_segments == 0u);
	BOOST_TEST(frame.points.size() == 4u);
}

BOOST_AUTO_TEST_CASE(WithXray_EveryEdgeAndVertexIsDrawn)
{
	const overlay_frame frame = build(true);
	BOOST_TEST(frame.segments.size() == 12u);
	BOOST_TEST(frame.points.size() == 8u);
}

BOOST_AUTO_TEST_CASE(OnlyTheVertexModeDrawsPoints)
{
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_TEST(build(true).points.empty());
	selection.set_mode(select_mode::face, mesh, *adjacency);
	BOOST_TEST(build(true).points.empty());
}

BOOST_AUTO_TEST_CASE(ASelectedVertex_IsMarkedAndDrawnLast)
{
	const vertex_id first = mesh.vertices().begin()->first;
	selection.apply_vertex(first, select_op::replace);
	const overlay_frame frame = build(true);
	BOOST_REQUIRE(!frame.points.empty());
	BOOST_TEST((frame.points.back().kind == overlay_kind::selected));
	BOOST_TEST(std::ranges::count_if(frame.points, [](const overlay_point& p) { return p.kind == overlay_kind::selected; }) == 1);
}

BOOST_AUTO_TEST_CASE(ASelectedFace_IsAPolygon_AndItsEdgesAreSelected)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(front_face(), select_op::replace);

	const overlay_frame frame = build();
	BOOST_REQUIRE_EQUAL(frame.polygons.size(), 1u);
	BOOST_TEST((frame.polygons.front().kind == overlay_kind::selected));
	BOOST_TEST(frame.polygons.front().corners.size() == 4u);
	BOOST_TEST(frame.polygons.front().triangles.size() == 2u);
	BOOST_TEST(count(frame, overlay_kind::selected) == 4u);
}

BOOST_AUTO_TEST_CASE(AFaceTurnedAway_HasNoPolygonWithoutXray)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	for (const auto& [id, f] : mesh.faces()) {
		if (id != front_face()) {
			selection.apply_face(id, select_op::add);
			break;
		}
	}
	BOOST_TEST(build().polygons.empty());
	BOOST_TEST(build(true).polygons.size() == 1u);
}

BOOST_AUTO_TEST_CASE(HoverIsBelowSelected_AndAHoveredSelectedFaceIsDrawnOnceAsSelected)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	hover.face = front_face();
	overlay_frame frame = build();
	BOOST_REQUIRE_EQUAL(frame.polygons.size(), 1u);
	BOOST_TEST((frame.polygons.front().kind == overlay_kind::hover));
	BOOST_TEST(count(frame, overlay_kind::hover) == 4u);

	selection.apply_face(front_face(), select_op::replace);
	frame = build();
	BOOST_REQUIRE_EQUAL(frame.polygons.size(), 1u);
	BOOST_TEST((frame.polygons.front().kind == overlay_kind::selected));
	BOOST_TEST(count(frame, overlay_kind::selected) == 4u);
	BOOST_TEST(count(frame, overlay_kind::hover) == 0u);
}

BOOST_AUTO_TEST_CASE(Segments_AreOrderedPlainThenHoverThenSelected)
{
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	selection.apply_edge(adjacency->edges()[0], select_op::add);
	hover.edge = adjacency->edges()[1];
	const overlay_frame frame = build(true);
	BOOST_REQUIRE_EQUAL(frame.segments.size(), 12u);
	BOOST_TEST((frame.segments[11].kind == overlay_kind::selected));
	BOOST_TEST((frame.segments[10].kind == overlay_kind::hover));
	BOOST_TEST((frame.segments[0].kind == overlay_kind::plain));
}

BOOST_AUTO_TEST_CASE(OverTheBudget_PlainEdgesGoFarthestFromTheCursorFirst_AndTheCountIsKept)
{
	const glm::vec2 top_middle = *view.project({ 0.f, 0.5f, 0.5f });
	const overlay_frame frame = build(true, 6, top_middle);
	BOOST_TEST(frame.segments.size() == 6u);
	BOOST_TEST(frame.omitted_segments == 6u);

	const glm::vec2 left = *view.project({ -0.5f, 0.5f, 0.5f });
	const glm::vec2 right = *view.project({ 0.5f, 0.5f, 0.5f });
	const bool near_the_cursor = std::ranges::any_of(frame.segments, [&](const overlay_segment& s) {
		return (glm::distance(s.from, left) < 0.5f && glm::distance(s.to, right) < 0.5f) || (glm::distance(s.from, right) < 0.5f && glm::distance(s.to, left) < 0.5f);
	});
	BOOST_TEST(near_the_cursor);
}

BOOST_AUTO_TEST_CASE(OverTheBudget_TheSelectedEdgesStayAll)
{
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	selection.select_all(mesh, *adjacency);
	const overlay_frame frame = build(true, 6);
	BOOST_TEST(frame.segments.size() == 12u);
	BOOST_TEST(frame.omitted_segments == 0u);
}

BOOST_AUTO_TEST_CASE(AnEdgeAcrossTheCameraPlane_IsDrawnByItsPartInFront_AndTheVertexBehindHasNoPoint)
{
	poly_mesh across;
	const vertex_id a = across.add_vertex({ -1.f, 0.f, 0.f });
	const vertex_id b = across.add_vertex({ -1.f, 0.f, 10.f });
	const vertex_id c = across.add_vertex({ -1.f, 1.f, 0.f });
	across.add_face({ { a }, { b }, { c } });
	const mesh_adjacency across_adjacency(across);
	const mesh_selection vertices{};

	overlay_options options;
	options.xray = true;
	const overlay_frame frame = build_overlay(across, across_adjacency, vertices, {}, view, options);
	BOOST_TEST(frame.segments.size() == 3u);
	BOOST_TEST(frame.points.size() == 2u);
}

BOOST_AUTO_TEST_CASE(AFloorYouStandOver_KeepsItsEdgesWithoutXray_ThoughItsCentreIsOutOfSight)
{
	poly_mesh floor;
	const vertex_id a = floor.add_vertex({ -5.f, 0.f, 5.f });
	const vertex_id b = floor.add_vertex({ 5.f, 0.f, 5.f });
	const vertex_id c = floor.add_vertex({ 5.f, 0.f, -5.f });
	const vertex_id d = floor.add_vertex({ -5.f, 0.f, -5.f });
	floor.add_face({ { a }, { b }, { c }, { d } });
	const mesh_adjacency floor_adjacency(floor);
	const glm::mat4 look = glm::lookAt(glm::vec3{ 0.f, 2.f, 0.f }, glm::vec3{ 0.f, 2.f, -1.f }, glm::vec3{ 0.f, 1.f, 0.f });
	const screen_view above(glm::perspective(glm::radians(60.f), 1.f, 0.1f, 100.f) * look, { 0.f, 0.f }, { k_image, k_image });
	BOOST_REQUIRE(!above.project({ 0.f, 0.f, 0.f }).has_value());

	const overlay_frame frame = build_overlay(floor, floor_adjacency, mesh_selection{}, {}, above);
	BOOST_TEST(frame.segments.size() == 3u);
}

BOOST_AUTO_TEST_CASE(OverTheBudget_TheDistanceIsToTheSegment_NotToItsMiddle)
{
	poly_mesh flat;
	const vertex_id a = flat.add_vertex({ -1.f, 0.f, 0.f });
	const vertex_id b = flat.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = flat.add_vertex({ 0.f, 0.1f, 0.f });
	flat.add_face({ { a }, { b }, { c } });
	const mesh_adjacency flat_adjacency(flat);
	const screen_view identity(glm::mat4(1.f), { 0.f, 0.f }, { k_image, k_image });

	overlay_options options;
	options.xray = true;
	options.segment_budget = 1;
	options.cursor = glm::vec2{ 20.f, 100.f };
	const overlay_frame frame = build_overlay(flat, flat_adjacency, mesh_selection{}, {}, identity, options);
	BOOST_REQUIRE_EQUAL(frame.segments.size(), 1u);
	BOOST_TEST(glm::distance(frame.segments.front().from, glm::vec2{ 0.f, 100.f }) < 0.01f);
	BOOST_TEST(glm::distance(frame.segments.front().to, glm::vec2{ 200.f, 100.f }) < 0.01f);
}

BOOST_AUTO_TEST_CASE(OverTheBudget_TheHoveredSegmentStays)
{
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	hover.edge = adjacency->edges()[5];
	const overlay_frame frame = build(true, 4, glm::vec2{ 0.f, 0.f });
	BOOST_TEST(frame.segments.size() == 4u);
	BOOST_TEST(count(frame, overlay_kind::hover) == 1u);
}

BOOST_AUTO_TEST_CASE(AHoveredEdge_IsShownInEdgeModeOnly)
{
	hover.edge = adjacency->edges()[0];
	BOOST_TEST(count(build(true), overlay_kind::hover) == 0u);
	selection.set_mode(select_mode::edge, mesh, *adjacency);
	BOOST_TEST(count(build(true), overlay_kind::hover) == 1u);
}

BOOST_AUTO_TEST_CASE(WithAPicker_TheTrianglesOfAFaceComeFromIt_AndAFaceItHasNoneOfHasNoPolygon)
{
	selection.set_mode(select_mode::face, mesh, *adjacency);
	selection.apply_face(front_face(), select_op::replace);
	const mesh_picker picker(mesh);
	BOOST_REQUIRE(picker.face_triangles(front_face()) != nullptr);
	BOOST_TEST(picker.face_triangles(front_face())->size() == 2u);
	BOOST_TEST(picker.face_triangles(9999u) == nullptr);

	overlay_options options;
	options.picker = &picker;
	const overlay_frame frame = build_overlay(mesh, *adjacency, selection, hover, view, options);
	BOOST_REQUIRE_EQUAL(frame.polygons.size(), 1u);
	BOOST_TEST(frame.polygons.front().triangles.size() == 2u);

	poly_mesh other;
	const mesh_picker other_picker(other);
	options.picker = &other_picker;
	BOOST_TEST(build_overlay(mesh, *adjacency, selection, hover, view, options).polygons.empty());
}

BOOST_AUTO_TEST_SUITE_END()
