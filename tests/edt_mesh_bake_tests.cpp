#include <boost/test/unit_test.hpp>
#include "edt_mesh_bake.h"
#include "edt_mesh_shapes.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
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
poly_mesh folded_strip(bool add_vertices_backwards = false, bool add_faces_backwards = false)
{
	const glm::vec3 positions[6] = { { 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, { 1.f, 0.f, 0.f }, { 1.f, 0.f, 1.f }, { 1.f, 1.f, 0.f }, { 1.f, 1.f, 1.f } };
	poly_mesh mesh;
	vertex_id ids[6];
	for (int step = 0; step < 6; ++step) {
		const int i = add_vertices_backwards ? 5 - step : step;
		ids[i] = mesh.add_vertex(positions[i]);
	}
	const vertex_id a0 = ids[0], a1 = ids[1], b0 = ids[2], b1 = ids[3], c0 = ids[4], c1 = ids[5];
	const std::vector<corner> flat_quad = { { a0, { 0.f, 0.f } }, { a1, { 0.f, 1.f } }, { b1, { 0.5f, 1.f } }, { b0, { 0.5f, 0.f } } };
	const std::vector<corner> raised_quad = { { b0, { 0.5f, 0.f } }, { b1, { 0.5f, 1.f } }, { c1, { 1.f, 1.f } }, { c0, { 1.f, 0.f } } };
	mesh.add_face(add_faces_backwards ? raised_quad : flat_quad);
	mesh.add_face(add_faces_backwards ? flat_quad : raised_quad);
	return mesh;
}

poly_mesh polygon(const std::vector<glm::vec3>& outline)
{
	poly_mesh mesh;
	std::vector<corner> corners;
	for (const glm::vec3& p : outline) {
		corners.push_back({ mesh.add_vertex(p) });
	}
	mesh.add_face(std::move(corners));
	return mesh;
}

float area_of_triangles(const baked_mesh& baked, const std::vector<baked_vertex>& vertices)
{
	float area = 0.f;
	for (std::size_t i = 0; i + 2 < baked.geometry.indices.size(); i += 3) {
		const glm::vec3& p0 = vertices[baked.geometry.indices[i]].position;
		area += glm::length(glm::cross(vertices[baked.geometry.indices[i + 1]].position - p0, vertices[baked.geometry.indices[i + 2]].position - p0)) * 0.5f;
	}
	return area;
}

poly_mesh star(std::uint32_t corners)
{
	std::vector<glm::vec3> outline;
	for (std::uint32_t i = 0; i < corners; ++i) {
		const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(corners);
		const float radius = i % 2 == 0 ? 1.f : 0.4f;
		outline.emplace_back(radius * std::cos(angle), 0.f, radius * std::sin(angle));
	}
	poly_mesh mesh;
	std::vector<corner> loop;
	for (std::size_t i = outline.size(); i-- > 0;) {
		loop.push_back({ mesh.add_vertex(outline[i]) });
	}
	mesh.add_face(std::move(loop));
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

bool inside_xy(const std::vector<glm::vec3>& outline, const glm::vec2& point)
{
	bool inside = false;
	for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
		const glm::vec3& a = outline[i];
		const glm::vec3& b = outline[j];
		if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) {
			inside = !inside;
		}
	}
	return inside;
}

// The L has a reflex corner D(1,1) on the diagonal F-B of the ear at A. Cutting the ear at B next would cover F-D-C, ground outside
// the face; the triangles must stay inside and add up to the area of the L.
BOOST_AUTO_TEST_CASE(AConcaveFace_WithAVertexOnAnEarsDiagonal_IsTriangulatedInsideItself_Anywhere)
{
	const std::vector<glm::vec3> base = { { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { 2.f, 1.f, 0.f }, { 1.f, 1.f, 0.f }, { 1.f, 2.f, 0.f }, { 0.f, 2.f, 0.f } };
	for (const glm::vec3& offset : { glm::vec3{ 0.f }, glm::vec3{ 1000.f, 1000.f, 1000.f } }) {
		for (const bool reversed : { false, true }) {
			BOOST_TEST_CONTEXT("offset " << offset.x << (reversed ? ", reversed" : "")) {
				std::vector<glm::vec3> outline;
				for (const glm::vec3& p : base) {
					outline.push_back(p + offset);
				}
				if (reversed) {
					std::ranges::reverse(outline);
				}

				const baked_mesh baked = bake(polygon(outline));
				const std::vector<baked_vertex> vertices = read_vertices(baked.geometry);

				BOOST_TEST(baked.skipped_faces.empty());
				BOOST_REQUIRE(baked.geometry.indices.size() == 12u);
				float area = 0.f;
				for (std::size_t i = 0; i < 12; i += 3) {
					const glm::vec3& p0 = vertices[baked.geometry.indices[i]].position;
					const glm::vec3& p1 = vertices[baked.geometry.indices[i + 1]].position;
					const glm::vec3& p2 = vertices[baked.geometry.indices[i + 2]].position;
					const glm::vec3 centroid = (p0 + p1 + p2) / 3.f;
					BOOST_TEST(inside_xy(outline, { centroid.x, centroid.y }));
					area += glm::length(glm::cross(p1 - p0, p2 - p0)) * 0.5f;
				}
				BOOST_TEST(std::abs(area - 3.f) < 1e-4f);
			}
		}
	}
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

BOOST_AUTO_TEST_CASE(TheBake_DoesNotDependOnTheIdsOfTheVertices_ButFollowsTheOrderOfTheFaces)
{
	const baked_mesh forwards = bake(folded_strip());
	const baked_mesh other_vertex_ids = bake(folded_strip(true));
	const baked_mesh other_face_order = bake(folded_strip(false, true));

	BOOST_TEST((forwards.geometry.vertices == other_vertex_ids.geometry.vertices));
	BOOST_TEST((forwards.geometry.indices == other_vertex_ids.geometry.indices));
	BOOST_TEST((forwards.geometry.vertices != other_face_order.geometry.vertices));
	BOOST_TEST(forwards.geometry.vertices.size() == other_face_order.geometry.vertices.size());
}

BOOST_AUTO_TEST_CASE(FacesThatAreNoSimplePolygon_AreLeftOutAndReported)
{
	const poly_mesh pentagram = polygon({ { 1.f, 0.f, 0.f }, { -0.81f, 0.f, 0.59f }, { 0.31f, 0.f, -0.95f }, { 0.31f, 0.f, 0.95f }, { -0.81f, 0.f, -0.59f } });
	const poly_mesh bowtie = polygon({ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 1.f }, { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f } });
	const poly_mesh collinear = polygon({ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { 3.f, 0.f, 0.f } });
	for (const poly_mesh* mesh : { &pentagram, &bowtie, &collinear }) {
		const baked_mesh baked = bake(*mesh);
		BOOST_TEST(baked.skipped_faces.size() == 1u);
		BOOST_TEST(baked.geometry.indices.empty());
	}
}

BOOST_AUTO_TEST_CASE(AFaceThatUsesAVertexTwice_OrHasAPositionThatIsNotFinite_IsLeftOutAndReported)
{
	poly_mesh twice;
	const vertex_id a = twice.add_vertex({ 0.f, 0.f, 0.f });
	const vertex_id b = twice.add_vertex({ 1.f, 0.f, 0.f });
	const vertex_id c = twice.add_vertex({ 1.f, 0.f, 1.f });
	const vertex_id d = twice.add_vertex({ 0.f, 0.f, 1.f });
	twice.add_face({ { a }, { b }, { c }, { b }, { d } });
	BOOST_TEST(bake(twice).skipped_faces.size() == 1u);

	poly_mesh infinite = polygon({ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f } });
	infinite.set_position(2, { std::numeric_limits<float>::infinity(), 0.f, 0.f });
	const baked_mesh baked = bake(infinite);
	BOOST_TEST(baked.skipped_faces.size() == 1u);
	BOOST_TEST(baked.geometry.vertices.empty());
}

BOOST_AUTO_TEST_CASE(AFaceWithAVertexOnItsEdge_IsBakedWithItsFullArea)
{
	const baked_mesh baked = bake(polygon({ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { 2.f, 0.f, 2.f }, { 0.f, 0.f, 2.f } }));

	BOOST_TEST(baked.skipped_faces.empty());
	BOOST_TEST(std::abs(area_of_triangles(baked, read_vertices(baked.geometry)) - 4.f) < 1e-4f);
}

BOOST_AUTO_TEST_CASE(AStarShapedFace_OfFewCorners_IsTriangulated_AndOfManyIsReported)
{
	const baked_mesh few = bake(star(16));
	BOOST_TEST(few.skipped_faces.empty());
	BOOST_TEST(few.geometry.indices.size() == 14u * 3u);

	const auto started = std::chrono::steady_clock::now();
	const baked_mesh many = bake(star(1024));
	BOOST_TEST(many.skipped_faces.size() == 1u);
	BOOST_TEST((std::chrono::steady_clock::now() - started < std::chrono::seconds(10)));
}

BOOST_AUTO_TEST_CASE(AHedgehog_OfManyFacesAroundOneVertex_IsBakedInTime)
{
	constexpr std::uint32_t spikes = 30000;
	poly_mesh mesh;
	const vertex_id centre = mesh.add_vertex({ 0.f, 0.f, 0.f });
	std::vector<vertex_id> ring;
	ring.reserve(spikes);
	for (std::uint32_t i = 0; i < spikes; ++i) {
		const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(spikes);
		ring.push_back(mesh.add_vertex({ std::cos(angle), 0.f, -std::sin(angle) }));
	}
	for (std::uint32_t i = 0; i < spikes; ++i) {
		mesh.add_face({ { centre }, { ring[i] }, { ring[(i + 1) % spikes] } });
	}

	const auto started = std::chrono::steady_clock::now();
	const baked_mesh baked = bake(mesh);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	BOOST_TEST(baked.skipped_faces.empty());
	BOOST_TEST(baked.geometry.indices.size() == spikes * 3u);
	BOOST_TEST((elapsed < std::chrono::seconds(20)));
}

BOOST_AUTO_TEST_CASE(AMeshWithoutUv_StillGetsAFiniteOrthonormalFrame)
{
	const baked_mesh baked = bake(polygon({ { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } }));

	for (const baked_vertex& v : read_vertices(baked.geometry)) {
		BOOST_TEST(std::isfinite(v.tangent.x + v.tangent.y + v.tangent.z + v.bitangent.x + v.bitangent.y + v.bitangent.z));
		BOOST_TEST(std::abs(glm::length(v.tangent) - 1.f) < 1e-4f);
		BOOST_TEST(std::abs(glm::length(v.bitangent) - 1.f) < 1e-4f);
		BOOST_TEST(std::abs(glm::dot(v.normal, v.tangent)) < 1e-4f);
		BOOST_TEST(std::abs(glm::dot(v.tangent, v.bitangent)) < 1e-4f);
	}
}

BOOST_AUTO_TEST_SUITE_END()
