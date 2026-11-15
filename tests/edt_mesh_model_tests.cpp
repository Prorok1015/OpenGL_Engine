#include <boost/test/unit_test.hpp>
#include "edt_mesh_adjacency.h"
#include "edt_mesh_model.h"
#include "edt_mesh_shapes.h"
#include <cmath>

namespace {

using namespace edt::mesh;

poly_mesh make(const shape_params& params)
{
	shape_result result = make_shape(params);
	BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
	return std::move(*result.mesh);
}

}

BOOST_AUTO_TEST_SUITE(EdtMeshModelTests)

BOOST_AUTO_TEST_CASE(AFace_NeedsThreeKnownVertices_AndNoneTwiceInARow)
{
	poly_mesh mesh;
	const vertex_id a = mesh.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = mesh.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = mesh.add_vertex({ 0.f, 1.f, 0.f });

	BOOST_TEST(mesh.add_face({ { a }, { b } }) == invalid_id);
	BOOST_TEST(mesh.add_face({ { a }, { b }, { b } }) == invalid_id);
	BOOST_TEST(mesh.add_face({ { a }, { b }, { a } }) == invalid_id);
	BOOST_TEST(mesh.add_face({ { a }, { b }, { 99 } }) == invalid_id);
	BOOST_TEST(mesh.add_face({ { a }, { b }, { c } }) != invalid_id);
	BOOST_TEST(mesh.validate().empty());
}

BOOST_AUTO_TEST_CASE(Ids_AreNotReused_AndSurviveARemoval)
{
	poly_mesh mesh;
	const vertex_id a = mesh.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = mesh.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = mesh.add_vertex({ 0.f, 1.f, 0.f });
	const face_id first = mesh.add_face({ { a }, { b }, { c } });

	BOOST_TEST(!mesh.remove_vertex(a));
	BOOST_TEST(mesh.remove_face(first));
	BOOST_TEST(mesh.remove_vertex(a));

	const vertex_id d = mesh.add_vertex({ 2.f, 0.f, 0.f });
	BOOST_TEST(d > c);
	BOOST_TEST(mesh.find_vertex(b) != nullptr);
	BOOST_TEST(mesh.find_vertex(a) == nullptr);
	const face_id second = mesh.add_face({ { b }, { c }, { d } });
	BOOST_TEST(second > first);
}

BOOST_AUTO_TEST_CASE(EveryChange_RaisesTheGeneration_AndStalesTheAdjacency)
{
	poly_mesh mesh = make(box_params{});
	const mesh_adjacency adjacency(mesh);
	BOOST_TEST(adjacency.is_current(mesh));

	mesh.set_position(1, { 5.f, 5.f, 5.f });

	BOOST_TEST(!adjacency.is_current(mesh));
}

BOOST_AUTO_TEST_CASE(Restore_KeepsTheGivenIds_AndRefusesATakenOne)
{
	poly_mesh mesh;
	BOOST_TEST(mesh.restore_vertex(7, { 0.f, 0.f, 0.f }));
	BOOST_TEST(!mesh.restore_vertex(7, { 1.f, 0.f, 0.f }));
	BOOST_TEST(!mesh.restore_vertex(invalid_id, { 1.f, 0.f, 0.f }));
	BOOST_TEST(mesh.add_vertex({ 2.f, 0.f, 0.f }) == 8u);
}

BOOST_AUTO_TEST_CASE(TheReservedId_IsNeverGivenOutNorRestored_AndTheIdsRunningOutGiveNoElement)
{
	poly_mesh mesh;
	BOOST_TEST(!mesh.restore_vertex(reserved_id, { 0.f, 0.f, 0.f }));
	BOOST_TEST(mesh.restore_vertex(reserved_id - 1, { 0.f, 0.f, 0.f }));
	BOOST_TEST(mesh.add_vertex({ 1.f, 0.f, 0.f }) == invalid_id);

	const vertex_id a = 1;
	BOOST_TEST(mesh.restore_vertex(a, { 1.f, 0.f, 0.f }));
	BOOST_TEST(mesh.restore_vertex(2, { 0.f, 1.f, 0.f }));
	BOOST_TEST(!mesh.restore_face(reserved_id, { { a }, { 2 }, { reserved_id - 1 } }, 0));
	BOOST_TEST(mesh.restore_face(reserved_id - 1, { { a }, { 2 }, { reserved_id - 1 } }, 0));
	BOOST_TEST(mesh.add_face({ { a }, { 2 }, { reserved_id - 1 } }) == invalid_id);
}

BOOST_AUTO_TEST_CASE(EdgeFlags_AreKeptPerEdge_NotPerDirection)
{
	poly_mesh mesh = make(box_params{});
	mesh.set_edge_flags(edge_key::of(2, 1), edge_flag::sharp | edge_flag::seam);

	BOOST_TEST(has_flag(mesh.edge_flags(edge_key::of(1, 2)), edge_flag::sharp));
	BOOST_TEST(has_flag(mesh.edge_flags(edge_key::of(1, 2)), edge_flag::seam));
	mesh.set_edge_flags(edge_key::of(1, 2), edge_flag::none);
	BOOST_TEST(mesh.flagged_edges().empty());
}

BOOST_AUTO_TEST_CASE(ABox_IsClosedAndManifold_WithEulerCharacteristicTwo)
{
	const poly_mesh mesh = make(box_params{});
	const mesh_adjacency adjacency(mesh);

	BOOST_TEST(mesh.vertices().size() == 8u);
	BOOST_TEST(mesh.faces().size() == 6u);
	BOOST_TEST(adjacency.edges().size() == 12u);
	BOOST_TEST(adjacency.euler_characteristic() == 2);
	BOOST_TEST(adjacency.non_manifold_edge_count() == 0u);
	for (const edge_key& key : adjacency.edges()) {
		BOOST_TEST(adjacency.faces_of_edge(key).size() == 2u);
		BOOST_TEST(!adjacency.is_boundary(key));
	}
	for (const auto& entry : mesh.vertices()) {
		BOOST_TEST(adjacency.edges_of_vertex(entry.first).size() == 3u);
		BOOST_TEST(adjacency.faces_of_vertex(entry.first).size() == 3u);
	}
}

BOOST_AUTO_TEST_CASE(APlane_HasABoundary_AndTheEulerCharacteristicOfADisc)
{
	const poly_mesh mesh = make(plane_params{ { 2.f, 2.f }, 3, 2 });
	const mesh_adjacency adjacency(mesh);

	BOOST_TEST(mesh.vertices().size() == 12u);
	BOOST_TEST(mesh.faces().size() == 6u);
	BOOST_TEST(adjacency.euler_characteristic() == 1);
	std::size_t boundary = 0;
	for (const edge_key& key : adjacency.edges()) {
		boundary += adjacency.is_boundary(key) ? 1u : 0u;
	}
	BOOST_TEST(boundary == 10u);
}

BOOST_AUTO_TEST_CASE(AnEdgeOfThreeFaces_IsNonManifold_AndTheMeshIsStillSound)
{
	poly_mesh mesh;
	const vertex_id a = mesh.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = mesh.add_vertex({ 1.f, 0.f, 0.f });
	for (int i = 0; i < 3; ++i) {
		const vertex_id apex = mesh.add_vertex({ 0.f, static_cast<float>(i + 1), 1.f });
		mesh.add_face({ { a }, { b }, { apex } });
	}

	const mesh_adjacency adjacency(mesh);
	BOOST_TEST(adjacency.non_manifold_edge_count() == 1u);
	BOOST_TEST(adjacency.is_non_manifold(edge_key::of(a, b)));
	BOOST_TEST(mesh.validate().empty());
}

BOOST_AUTO_TEST_CASE(Shapes_HaveTheCountsTheirParametersGive)
{
	const poly_mesh sphere = make(sphere_params{ 0.5f, 12, 6 });
	BOOST_TEST(sphere.vertices().size() == 12u * 5u + 2u);
	BOOST_TEST(sphere.faces().size() == 12u * 6u);
	BOOST_TEST(mesh_adjacency(sphere).euler_characteristic() == 2);

	const poly_mesh cylinder = make(cylinder_params{ 0.5f, 2.f, 10 });
	BOOST_TEST(cylinder.vertices().size() == 20u);
	BOOST_TEST(cylinder.faces().size() == 12u);
	BOOST_TEST(mesh_adjacency(cylinder).euler_characteristic() == 2);

	const poly_mesh capsule = make(capsule_params{ 0.5f, 1.f, 8, 3 });
	BOOST_TEST(capsule.vertices().size() == 8u * 6u + 2u);
	BOOST_TEST(capsule.faces().size() == 8u * 7u);
	BOOST_TEST(mesh_adjacency(capsule).euler_characteristic() == 2);
}

BOOST_AUTO_TEST_CASE(EveryShape_IsSound_WithUvInTheUnitSquare)
{
	const shape_params all[] = { box_params{}, plane_params{}, sphere_params{}, cylinder_params{}, capsule_params{} };
	for (const shape_params& params : all) {
		BOOST_TEST_CONTEXT(shape_kind_name(params)) {
			const poly_mesh mesh = make(params);
			BOOST_TEST(mesh.validate().empty());
			BOOST_TEST(mesh_adjacency(mesh).non_manifold_edge_count() == 0u);
			for (const auto& entry : mesh.faces()) {
				for (const corner& c : entry.second.corners) {
					BOOST_TEST(c.uv.x >= 0.f);
					BOOST_TEST(c.uv.x <= 1.f);
					BOOST_TEST(c.uv.y >= 0.f);
					BOOST_TEST(c.uv.y <= 1.f);
				}
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(AShapeParameterOutOfRange_GivesNoMesh_AndAnError)
{
	BOOST_TEST(!make_shape(box_params{ { 1.f, 0.f, 1.f } }).mesh.has_value());
	BOOST_TEST(!make_shape(sphere_params{ -1.f, 16, 8 }).mesh.has_value());
	BOOST_TEST(!make_shape(sphere_params{ 1.f, 2, 8 }).mesh.has_value());
	BOOST_TEST(!make_shape(cylinder_params{ 1.f, 1.f, max_shape_segments + 1 }).mesh.has_value());
	BOOST_TEST(!make_shape(plane_params{ { 1.f, 1.f }, 0, 1 }).mesh.has_value());
	BOOST_TEST(!make_shape(capsule_params{ 1.f, 0.f, 8, 2 }).mesh.has_value());
	BOOST_TEST(!make_shape(box_params{ { std::nanf(""), 1.f, 1.f } }).mesh.has_value());
	BOOST_TEST(!make_shape(capsule_params{ 3.0e38f, 1.f, 8, 2 }).mesh.has_value());
	BOOST_TEST(!make_shape(box_params{ { 2.0e6f, 1.f, 1.f } }).mesh.has_value());
	BOOST_TEST(make_shape(sphere_params{ 1.f, 2, 8 }).error.find("sphere") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ShapeKinds_AreNamedAndFoundByName)
{
	for (const char* kind : { "box", "plane", "sphere", "cylinder", "capsule" }) {
		const auto params = default_shape_params(kind);
		BOOST_REQUIRE(params.has_value());
		BOOST_TEST(shape_kind_name(*params) == kind);
	}
	BOOST_TEST(!default_shape_params("torus").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
