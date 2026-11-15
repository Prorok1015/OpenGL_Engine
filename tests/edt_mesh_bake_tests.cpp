#include <boost/test/unit_test.hpp>
#include "edt_mesh_bake.h"
#include "edt_mesh_shapes.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace edt::mesh;

struct baked_vertex
{
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec2 uv;
	glm::vec3 tangent;
	glm::vec3 bitangent;
};

constexpr std::size_t stride = 14 * sizeof(float);

std::vector<baked_vertex> read_vertices(const rnd::geometry_desc& geometry)
{
	BOOST_REQUIRE(geometry.vertices.size() % stride == 0);
	std::vector<baked_vertex> out(geometry.vertices.size() / stride);
	for (std::size_t i = 0; i < out.size(); ++i) {
		float f[14];
		std::memcpy(f, geometry.vertices.data() + i * stride, stride);
		out[i] = { { f[0], f[1], f[2] }, { f[3], f[4], f[5] }, { f[6], f[7] }, { f[8], f[9], f[10] }, { f[11], f[12], f[13] } };
	}
	return out;
}

poly_mesh make(const shape_params& params)
{
	shape_result result = make_shape(params);
	BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
	return std::move(*result.mesh);
}

bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
{
	return glm::length(a - b) < eps;
}

// Two quads sharing the edge b0-b1, the second one standing up at 90 degrees; the UV runs on across the edge.
poly_mesh folded_strip()
{
	poly_mesh mesh;
	const vertex_id a0 = mesh.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id a1 = mesh.add_vertex({ 0.f, 0.f, 1.f });
	const vertex_id b0 = mesh.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id b1 = mesh.add_vertex({ 1.f, 0.f, 1.f });
	const vertex_id c0 = mesh.add_vertex({ 1.f, 1.f, 0.f });
	const vertex_id c1 = mesh.add_vertex({ 1.f, 1.f, 1.f });
	mesh.add_face({ { a0, { 0.f, 0.f } }, { a1, { 0.f, 1.f } }, { b1, { 0.5f, 1.f } }, { b0, { 0.5f, 0.f } } });
	mesh.add_face({ { b0, { 0.5f, 0.f } }, { b1, { 0.5f, 1.f } }, { c1, { 1.f, 1.f } }, { c0, { 1.f, 0.f } } });
	return mesh;
}

}

BOOST_AUTO_TEST_SUITE(EdtMeshBakeTests)

BOOST_AUTO_TEST_CASE(TwoBakes_OfTheSameMesh_AreByteForByteEqual)
{
	const shape_params all[] = { box_params{}, plane_params{ { 2.f, 1.f }, 3, 2 }, sphere_params{}, cylinder_params{}, capsule_params{} };
	for (const shape_params& params : all) {
		BOOST_TEST_CONTEXT(shape_kind_name(params)) {
			const baked_mesh first = bake(make(params));
			const baked_mesh second = bake(make(params));

			BOOST_TEST(!first.geometry.vertices.empty());
			BOOST_TEST((first.geometry.vertices == second.geometry.vertices));
			BOOST_TEST((first.geometry.indices == second.geometry.indices));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheLayout_HasAllFiveAttributes_InTheOrderOfTheShaderInputs)
{
	const baked_mesh baked = bake(make(box_params{}));

	const auto& elements = baked.geometry.layout.get_elements();
	const char* names[] = { "position", "normal", "uv", "tangent", "bitangent" };
	BOOST_REQUIRE(elements.size() == 5u);
	for (std::size_t i = 0; i < 5; ++i) {
		BOOST_TEST(elements[i].Name == names[i]);
	}
	BOOST_TEST(baked.geometry.layout.get_stride() == stride);
	BOOST_TEST(baked.geometry.bounds.empty());
}

BOOST_AUTO_TEST_CASE(ABox_GetsHardEdges_TwentyFourVerticesAndThirtySixIndices)
{
	const baked_mesh baked = bake(make(box_params{}));

	BOOST_TEST(read_vertices(baked.geometry).size() == 24u);
	BOOST_TEST(baked.geometry.indices.size() == 36u);
}

BOOST_AUTO_TEST_CASE(ASphere_IsSmooth_AndSplitsOnlyOnTheUvSeamAndAtThePoles)
{
	const baked_mesh baked = bake(make(sphere_params{ 0.5f, 16, 8 }));

	BOOST_TEST(read_vertices(baked.geometry).size() == 7u * 17u + 16u + 16u);
	BOOST_TEST(baked.geometry.indices.size() == (16u + 96u * 2u + 16u) * 3u);
}

BOOST_AUTO_TEST_CASE(EveryVertex_HasAUnitNormalAndAUnitTangentFrame_AndUvInTheUnitSquare)
{
	const shape_params all[] = { box_params{}, plane_params{}, sphere_params{}, cylinder_params{}, capsule_params{} };
	for (const shape_params& params : all) {
		BOOST_TEST_CONTEXT(shape_kind_name(params)) {
			const baked_mesh baked = bake(make(params));
			for (const baked_vertex& v : read_vertices(baked.geometry)) {
				BOOST_TEST(std::abs(glm::length(v.normal) - 1.f) < 1e-4f);
				BOOST_TEST(std::abs(glm::length(v.tangent) - 1.f) < 1e-4f);
				BOOST_TEST(std::abs(glm::length(v.bitangent) - 1.f) < 1e-4f);
				BOOST_TEST(std::abs(glm::dot(v.normal, v.tangent)) < 1e-4f);
				BOOST_TEST(std::abs(glm::dot(v.normal, v.bitangent)) < 1e-4f);
				BOOST_TEST(v.uv.x >= 0.f);
				BOOST_TEST(v.uv.x <= 1.f);
				BOOST_TEST(v.uv.y >= 0.f);
				BOOST_TEST(v.uv.y <= 1.f);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(NormalsOfClosedShapes_PointOutward)
{
	const shape_params closed[] = { box_params{}, sphere_params{}, capsule_params{} };
	for (const shape_params& params : closed) {
		BOOST_TEST_CONTEXT(shape_kind_name(params)) {
			const baked_mesh baked = bake(make(params));
			for (const baked_vertex& v : read_vertices(baked.geometry)) {
				BOOST_TEST(glm::dot(v.normal, v.position) > 0.f);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(TheTriangles_OfAClosedShape_WindCounterClockwiseSeenFromOutside)
{
	const baked_mesh baked = bake(make(cylinder_params{}));
	const std::vector<baked_vertex> vertices = read_vertices(baked.geometry);

	for (std::size_t i = 0; i + 2 < baked.geometry.indices.size(); i += 3) {
		const glm::vec3& p0 = vertices[baked.geometry.indices[i]].position;
		const glm::vec3& p1 = vertices[baked.geometry.indices[i + 1]].position;
		const glm::vec3& p2 = vertices[baked.geometry.indices[i + 2]].position;
		const glm::vec3 centre = (p0 + p1 + p2) / 3.f;
		BOOST_TEST(glm::dot(glm::cross(p1 - p0, p2 - p0), centre) > 0.f);
	}
}

BOOST_AUTO_TEST_CASE(ThePlaneTangent_RunsAlongU_AndTheBitangentAlongV)
{
	const baked_mesh baked = bake(make(plane_params{}));

	for (const baked_vertex& v : read_vertices(baked.geometry)) {
		BOOST_TEST(near(v.normal, { 0.f, 1.f, 0.f }));
		BOOST_TEST(near(v.tangent, { 1.f, 0.f, 0.f }));
		BOOST_TEST(near(v.bitangent, { 0.f, 0.f, 1.f }));
	}
}

BOOST_AUTO_TEST_CASE(AFold_SharesItsNormals_OnlyWithinTheSmoothAngle)
{
	const poly_mesh mesh = folded_strip();

	const baked_mesh hard = bake(mesh, { 45.f });
	BOOST_TEST(read_vertices(hard.geometry).size() == 8u);

	const baked_mesh smooth = bake(mesh, { 120.f });
	const std::vector<baked_vertex> vertices = read_vertices(smooth.geometry);
	BOOST_REQUIRE(vertices.size() == 6u);
	const glm::vec3 expected = glm::normalize(glm::vec3{ -1.f, 1.f, 0.f });
	std::size_t on_the_fold = 0;
	for (const baked_vertex& v : vertices) {
		if (std::abs(v.position.x - 1.f) < 1e-6f && std::abs(v.position.y) < 1e-6f) {
			BOOST_TEST(near(v.normal, expected));
			++on_the_fold;
		}
	}
	BOOST_TEST(on_the_fold == 2u);
}

BOOST_AUTO_TEST_CASE(ASharpEdge_SplitsTheVertices_EvenInAFlatMesh)
{
	poly_mesh mesh = make(plane_params{ { 2.f, 1.f }, 2, 1 });
	BOOST_TEST(read_vertices(bake(mesh).geometry).size() == 6u);

	mesh.set_edge_flags(edge_key::of(2, 5), edge_flag::sharp);

	const baked_mesh baked = bake(mesh);
	BOOST_TEST(read_vertices(baked.geometry).size() == 8u);
	BOOST_TEST(baked.geometry.indices.size() == 12u);
}

BOOST_AUTO_TEST_CASE(AFaceWithoutArea_IsLeftOut_AndReported)
{
	poly_mesh mesh;
	const vertex_id a = mesh.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = mesh.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = mesh.add_vertex({ 2.f, 0.f, 0.f });
	const face_id flat = mesh.add_face({ { a }, { b }, { c } });

	const baked_mesh baked = bake(mesh);

	BOOST_TEST(baked.geometry.indices.empty());
	BOOST_TEST(baked.geometry.vertices.empty());
	BOOST_REQUIRE(baked.skipped_faces.size() == 1u);
	BOOST_TEST(baked.skipped_faces[0] == flat);
	BOOST_TEST(baked.ranges.empty());
}

BOOST_AUTO_TEST_CASE(AConcaveFace_IsTriangulatedWithoutLosingItsArea)
{
	poly_mesh mesh;
	const glm::vec3 outline[] = { { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { 2.f, 1.f, 0.f }, { 1.f, 1.f, 0.f }, { 1.f, 2.f, 0.f }, { 0.f, 2.f, 0.f } };
	std::vector<corner> corners;
	for (const glm::vec3& p : outline) {
		corners.push_back({ mesh.add_vertex(p) });
	}
	mesh.add_face(std::move(corners));

	const baked_mesh baked = bake(mesh);
	const std::vector<baked_vertex> vertices = read_vertices(baked.geometry);

	BOOST_REQUIRE(baked.geometry.indices.size() == 12u);
	float area = 0.f;
	for (std::size_t i = 0; i < 12; i += 3) {
		const glm::vec3& p0 = vertices[baked.geometry.indices[i]].position;
		const glm::vec3& p1 = vertices[baked.geometry.indices[i + 1]].position;
		const glm::vec3& p2 = vertices[baked.geometry.indices[i + 2]].position;
		const float signed_area = glm::cross(p1 - p0, p2 - p0).z * 0.5f;
		BOOST_TEST(signed_area > 0.f);
		area += signed_area;
	}
	BOOST_TEST(std::abs(area - 3.f) < 1e-4f);
}

BOOST_AUTO_TEST_CASE(MaterialSlots_GetTheirOwnRanges_WithIndicesCountedFromTheRangeStart)
{
	poly_mesh mesh;
	const vertex_id a = mesh.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = mesh.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = mesh.add_vertex({ 0.f, 1.f, 0.f });
	const vertex_id d = mesh.add_vertex({ 1.f, 1.f, 0.f });
	mesh.add_face({ { a }, { b }, { c } }, 1);
	mesh.add_face({ { b }, { d }, { c } }, 0);

	const baked_mesh baked = bake(mesh);

	BOOST_REQUIRE(baked.ranges.size() == 2u);
	BOOST_TEST(baked.ranges[0].slot == 0u);
	BOOST_TEST(baked.ranges[1].slot == 1u);
	BOOST_TEST(baked.ranges[0].vx_begin == 0u);
	BOOST_TEST(baked.ranges[0].vx_end == 3u);
	BOOST_TEST(baked.ranges[1].vx_begin == 3u);
	BOOST_TEST(baked.ranges[1].vx_end == 6u);
	BOOST_TEST(baked.ranges[1].ind_begin == 3u);
	BOOST_TEST(baked.ranges[1].ind_end == 6u);
	for (std::size_t i = baked.ranges[1].ind_begin; i < baked.ranges[1].ind_end; ++i) {
		BOOST_TEST(baked.geometry.indices[i] < 3u);
	}
}

BOOST_AUTO_TEST_SUITE_END()
