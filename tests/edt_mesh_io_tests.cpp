#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "edt_mesh_bake.h"
#include "edt_mesh_io.h"
#include "edt_mesh_shapes.h"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_diagnostics.h>
#include <boost/json.hpp>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace edt::mesh;

struct mesh_io_fixture : test_util::null_engine_fixture
{
	mesh_io_fixture() : null_engine_fixture(false) {}

	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	template<class T>
	res::res_handle<T> load(std::string_view name, const std::string& text)
	{
		const res::tag tag{ std::string("memory://mesh_io/") + std::string(name) };
		BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(text)));
		auto handle = resources().require_sync<T>(tag);
		test_util::pump(tasks);
		return handle;
	}
};

poly_mesh_desc shape_desc(const shape_params& params)
{
	shape_result result = make_shape(params);
	BOOST_REQUIRE_MESSAGE(result.mesh.has_value(), result.error);
	poly_mesh_desc desc;
	desc.mesh = std::move(*result.mesh);
	desc.generator = params;
	return desc;
}

std::string load_error_of(const std::string& text, const poly_read_limits& limits = {})
{
	poly_mesh_desc out;
	try {
		poly_mesh_from_json(boost::json::parse(text).as_object(), out, limits);
	} catch (const res::load_error& error) {
		return error.what();
	}
	return {};
}

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshIoTests, mesh_io_fixture)

BOOST_AUTO_TEST_CASE(APolyMesh_SurvivesAWriteAndARead_WithItsIdsFlagsAndGenerator)
{
	poly_mesh_desc source = shape_desc(sphere_params{ 0.75f, 9, 5 });
	source.mesh.set_edge_flags(edge_key::of(1, 2), edge_flag::sharp);
	source.mesh.set_edge_flags(edge_key::of(2, 3), edge_flag::seam);
	source.mesh.remove_face(3);
	source.mesh.set_position(4, { 0.1f, -2.25f, 1.f / 3.f });

	poly_mesh_desc loaded;
	poly_mesh_from_json(boost::json::parse(boost::json::serialize(poly_mesh_to_json(source))).as_object(), loaded);

	BOOST_REQUIRE(loaded.mesh.vertices().size() == source.mesh.vertices().size());
	BOOST_REQUIRE(loaded.mesh.faces().size() == source.mesh.faces().size());
	for (const auto& [id, vertex_data] : source.mesh.vertices()) {
		const vertex* other = loaded.mesh.find_vertex(id);
		BOOST_REQUIRE(other);
		BOOST_TEST((other->position == vertex_data.position));
	}
	for (const auto& [id, face_data] : source.mesh.faces()) {
		const face* other = loaded.mesh.find_face(id);
		BOOST_REQUIRE(other);
		BOOST_REQUIRE(other->corners.size() == face_data.corners.size());
		for (std::size_t i = 0; i < face_data.corners.size(); ++i) {
			BOOST_TEST(other->corners[i].vertex == face_data.corners[i].vertex);
			BOOST_TEST((other->corners[i].uv == face_data.corners[i].uv));
		}
	}
	BOOST_TEST((loaded.mesh.flagged_edges() == source.mesh.flagged_edges()));
	BOOST_REQUIRE(loaded.generator.has_value());
	const auto* sphere = std::get_if<sphere_params>(&*loaded.generator);
	BOOST_REQUIRE(sphere);
	BOOST_TEST(sphere->radius == 0.75f);
	BOOST_TEST(sphere->segments == 9u);
	BOOST_TEST(sphere->rings == 5u);
	BOOST_TEST(loaded.mesh.add_vertex({}) == source.mesh.vertices().rbegin()->first + 1);
}

BOOST_AUTO_TEST_CASE(TheGeneratorOfEveryShape_SurvivesAWriteAndARead)
{
	const shape_params all[] = { box_params{ { 1.f, 2.f, 3.f } }, plane_params{ { 4.f, 5.f }, 2, 3 }, sphere_params{}, cylinder_params{ 1.f, 3.f, 7 },
		capsule_params{ 0.25f, 2.f, 6, 2 } };
	for (const shape_params& params : all) {
		BOOST_TEST_CONTEXT(shape_kind_name(params)) {
			poly_mesh_desc loaded;
			poly_mesh_from_json(poly_mesh_to_json(shape_desc(params)), loaded);

			BOOST_REQUIRE(loaded.generator.has_value());
			BOOST_TEST(shape_kind_name(*loaded.generator) == shape_kind_name(params));
			BOOST_TEST((loaded.generator->index() == params.index()));
			BOOST_TEST(boost::json::serialize(poly_mesh_to_json(loaded)) == boost::json::serialize(poly_mesh_to_json(shape_desc(params))));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheWrittenFile_IsTheSameTextEveryTime)
{
	const std::string first = boost::json::serialize(poly_mesh_to_json(shape_desc(capsule_params{})));
	const std::string second = boost::json::serialize(poly_mesh_to_json(shape_desc(capsule_params{})));

	BOOST_TEST(first == second);
}

BOOST_AUTO_TEST_CASE(APolyDesc_LoadsThroughTheResourceSystem_OnceItsTypeIsRegistered)
{
	auto& descs = storage.require<desc::desc_system>();
	register_mesh_descs(descs);

	const auto loaded = load<poly_mesh_desc>("box.poly.desc", boost::json::serialize(poly_mesh_to_json(shape_desc(box_params{}))));

	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	const auto desc = loaded.get_sync();
	BOOST_REQUIRE(desc);
	BOOST_TEST(desc->mesh.faces().size() == 6u);
	unregister_mesh_descs(descs);
}

BOOST_AUTO_TEST_CASE(TheGeometryFile_HasNoBounds_AndItsParentIsOptional)
{
	const baked_mesh baked = bake(*make_shape(box_params{}).mesh);

	const boost::json::object with_parent = boost::json::parse(write_geometry_json(baked.geometry)).as_object();
	BOOST_TEST(!with_parent.contains("bounds"));
	BOOST_TEST(std::string(with_parent.at("__parent").as_string()) == "res://base_geometry.desc");
	BOOST_TEST(std::string(with_parent.at("__type").as_string()) == "geometry_desc");

	const boost::json::object without = boost::json::parse(write_geometry_json(baked.geometry, { false })).as_object();
	BOOST_TEST(!without.contains("__parent"));
}

BOOST_AUTO_TEST_CASE(TheGeometryFile_LoadsInTheEngine_WithoutTheEditor_AndKeepsTheLayoutOrder)
{
	const baked_mesh baked = bake(*make_shape(sphere_params{ 0.5f, 8, 4 }).mesh);

	const auto loaded = load<rnd::geometry_desc>("sphere.geom.desc", write_geometry_json(baked.geometry, { false }));

	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	const auto geometry = loaded.get_sync();
	BOOST_REQUIRE(geometry);
	BOOST_TEST((geometry->vertices == baked.geometry.vertices));
	BOOST_TEST((geometry->indices == baked.geometry.indices));
	const auto& elements = geometry->layout.get_elements();
	const char* names[] = { "position", "normal", "uv", "tangent", "bitangent" };
	BOOST_REQUIRE(elements.size() == 5u);
	for (std::size_t i = 0; i < 5; ++i) {
		BOOST_TEST(elements[i].Name == names[i]);
	}
	BOOST_TEST(geometry->layout.get_stride() == baked.geometry.layout.get_stride());
}

BOOST_AUTO_TEST_CASE(AFileOverTheLimits_FailsWithAMessageThatNamesThePlace)
{
	poly_read_limits small;
	small.vertices = 3;
	small.faces = 2;
	small.corners = 5;
	small.face_corners = 4;
	small.edges = 1;
	small.material_slot = 7;

	const std::string four_vertices = R"({"vertices":[[1,0,0,0],[2,0,0,0],[3,0,0,0],[4,0,0,0]]})";
	BOOST_TEST(load_error_of(four_vertices, small).find("vertices holds 4 vertices, at most 3") != std::string::npos);

	const std::string vertices = R"("vertices":[[1,0,0,0],[2,1,0,0],[3,0,1,0]],)";
	const std::string three_faces = "{" + vertices + R"("faces":[{"id":1,"corners":[[1,0,0],[2,0,0],[3,0,0]]},{"id":2,"corners":[[1,0,0],[2,0,0],[3,0,0]]},{"id":3,"corners":[[1,0,0],[2,0,0],[3,0,0]]}]})";
	BOOST_TEST(load_error_of(three_faces, small).find("faces holds 3 faces, at most 2") != std::string::npos);

	const std::string two_faces = "{" + vertices + R"("faces":[{"id":1,"corners":[[1,0,0],[2,0,0],[3,0,0]]},{"id":2,"corners":[[1,0,0],[2,0,0],[3,0,0]]}]})";
	BOOST_TEST(load_error_of(two_faces, small).find("faces[1] is past the limit of 5 corners") != std::string::npos);

	const std::string five_corners = "{" + vertices + R"("faces":[{"id":1,"corners":[[1,0,0],[2,0,0],[3,0,0],[1,0,0],[2,0,0]]}]})";
	BOOST_TEST(load_error_of(five_corners, small).find("faces[0].corners holds 5 corners, at most 4") != std::string::npos);

	const std::string two_edges = "{" + vertices + R"("edges":[[1,2,1],[2,3,1]]})";
	BOOST_TEST(load_error_of(two_edges, small).find("edges holds 2 entries, at most 1") != std::string::npos);

	const std::string slot = "{" + vertices + R"("faces":[{"id":1,"material":8,"corners":[[1,0,0],[2,0,0],[3,0,0]]}]})";
	BOOST_TEST(load_error_of(slot, small).find("faces[0].material is 8, at most 7") != std::string::npos);

	BOOST_TEST(load_error_of(four_vertices).empty());
}

BOOST_AUTO_TEST_CASE(APositionThatIsInfiniteOrNotANumber_FailsTheLoad)
{
	for (const double bad : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1e39 }) {
		boost::json::object resource;
		boost::json::array vertex{ 1, 0, 0, bad };
		boost::json::array vertices;
		vertices.push_back(std::move(vertex));
		resource["vertices"] = std::move(vertices);
		poly_mesh_desc out;
		try {
			poly_mesh_from_json(resource, out);
			BOOST_FAIL("a position that is not a finite float was read");
		} catch (const res::load_error& error) {
			BOOST_TEST(std::string(error.what()).find("vertices[0][3] is not a finite float") != std::string::npos);
		}
	}
}

BOOST_AUTO_TEST_CASE(TheReservedId_IsRefusedWhenRead)
{
	BOOST_TEST(load_error_of(R"({"vertices":[[4294967295,0,0,0]]})").find("vertices[0]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,0,0],[2,0,0,0],[3,0,0,0]],"faces":[{"id":4294967295,"corners":[[1,0,0],[2,0,0],[3,0,0]]}]})")
		.find("faces[0]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ABakeOfAMeshRead_FromItsFile_IsTheBakeOfTheMeshItself)
{
	const poly_mesh_desc source = shape_desc(capsule_params{ 0.4f, 1.5f, 10, 3 });
	poly_mesh_desc loaded;
	poly_mesh_from_json(boost::json::parse(boost::json::serialize(poly_mesh_to_json(source))).as_object(), loaded);

	const baked_mesh original = bake(source.mesh);
	const baked_mesh read_back = bake(loaded.mesh);

	BOOST_TEST((original.geometry.vertices == read_back.geometry.vertices));
	BOOST_TEST((original.geometry.indices == read_back.geometry.indices));
}

BOOST_AUTO_TEST_CASE(AFileWithBrokenValues_FailsWithAMessageThatNamesThePlace)
{
	BOOST_TEST(load_error_of(R"({"vertices":{}})").find("vertices is not an array") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,0]]})").find("vertices[0]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,"x",0]]})").find("vertices[0][2] is not a number") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,0,0],[1,1,1,1]]})").find("vertices[1]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[0,0,0,0]]})").find("vertices[0]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[-1,0,0,0]]})").find("vertices[0][0]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,0,1e39]]})").find("vertices[0][3]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,0,0]],"faces":[{"id":1,"corners":[[1,0,0],[2,0,0],[3,0,0]]}]})").find("faces[0]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"vertices":[[1,0,0,0]],"edges":[[1,2,1]]})").find("edges[0]") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"generator":{"kind":"torus"}})").find("unknown shape kind") != std::string::npos);
	BOOST_TEST(load_error_of(R"({"generator":{"params":{}}})").find("generator.kind") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnEmptyFile_IsAnEmptyMesh)
{
	poly_mesh_desc out;
	poly_mesh_from_json(boost::json::parse(R"({"__type":"poly_mesh_desc"})").as_object(), out);

	BOOST_TEST(out.mesh.vertices().empty());
	BOOST_TEST(out.mesh.faces().empty());
	BOOST_TEST(!out.generator.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
