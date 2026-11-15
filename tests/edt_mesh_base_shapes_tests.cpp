#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "edt_mesh_base_shapes.h"
#include <snk/desc/desc_system.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace {

using namespace edt::mesh;

namespace fs = std::filesystem;

constexpr std::size_t vertex_stride = 14 * sizeof(float);

// A checkout on Windows may turn the file's line ends into CR LF; the text the generator writes has LF only.
std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::erase(text, '\r');
	return text;
}

struct bounds
{
	glm::vec3 min{ 1e30f };
	glm::vec3 max{ -1e30f };
};

bounds bounds_of(const rnd::geometry_desc& geometry)
{
	bounds out;
	for (std::size_t i = 0; i + vertex_stride <= geometry.vertices.size(); i += vertex_stride) {
		float position[3];
		std::memcpy(position, geometry.vertices.data() + i, sizeof(position));
		const glm::vec3 p{ position[0], position[1], position[2] };
		out.min = glm::min(out.min, p);
		out.max = glm::max(out.max, p);
	}
	return out;
}

baked_mesh bake_shape(const base_shape& shape)
{
	shape_result generated = make_shape(shape.params);
	BOOST_REQUIRE_MESSAGE(generated.mesh.has_value(), generated.error);
	return bake(*generated.mesh);
}

struct shapes_fixture : test_util::null_engine_fixture
{
	shapes_fixture() : null_engine_fixture(false) {}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtBaseShapesTests, shapes_fixture)

// A mismatch writes what the bake gives to generated_shapes/ next to the test binary: the Linux Clang job of CI uploads that
// folder as an artifact, and its files go into core/res/shapes unchanged (no one builds locally).
BOOST_AUTO_TEST_CASE(TheCommittedFiles_AreWhatTheBakeWrites)
{
	for (const base_shape& shape : base_shapes()) {
		BOOST_TEST_CONTEXT(shape.name) {
			const std::string expected = base_shape_file_text(shape);
			const fs::path committed = fs::path(SNK_CORE_RES_DIR) / "shapes" / (std::string(shape.name) + ".geom.desc");
			if (read_text(committed) != expected) {
				const fs::path generated = fs::path("generated_shapes") / committed.filename();
				fs::create_directories(generated.parent_path());
				std::ofstream(generated, std::ios::binary) << expected;
				BOOST_ERROR("core/res/shapes/" + committed.filename().string() + " is missing or differs from the generator: take the file from the generated_shapes artifact");
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(TheFileVersion_IsTheEnginesGeometryVersion_AndTheTextIsPrettyWithANewline)
{
	BOOST_TEST(base_shape_file_version == storage.require<desc::desc_system>().current_version("geometry_desc"));
	for (const base_shape& shape : base_shapes()) {
		const std::string text = base_shape_file_text(shape);
		BOOST_TEST(text.starts_with("{\n    \"__type\" : \"geometry_desc\",\n    \"__version\" : 1,\n"));
		BOOST_TEST(text.back() == '\n');
		BOOST_TEST(text.find("__parent") == std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(EveryShape_HasItsOwnName_AndPointsAtAGeomDescFile)
{
	std::set<std::string_view> names;
	for (const base_shape& shape : base_shapes()) {
		BOOST_TEST(names.insert(shape.name).second);
		const std::string path = base_shape_path(shape);
		BOOST_TEST(path.starts_with("res://shapes/"));
		BOOST_TEST(path.ends_with(".geom.desc"));
		BOOST_TEST(find_base_shape(shape.name) == &shape);
	}
	BOOST_TEST(find_base_shape("torus") == nullptr);
}

BOOST_AUTO_TEST_CASE(TheRange_IsTheWholeFile)
{
	for (const base_shape& shape : base_shapes()) {
		BOOST_TEST_CONTEXT(shape.name) {
			const baked_mesh baked = bake_shape(shape);
			const material_range range = base_shape_range(shape);
			BOOST_TEST(range.vx_begin == 0u);
			BOOST_TEST(range.ind_begin == 0u);
			BOOST_TEST(range.vx_end * vertex_stride == baked.geometry.vertices.size());
			BOOST_TEST(range.ind_end == baked.geometry.indices.size());
			BOOST_TEST(std::ranges::all_of(baked.geometry.indices, [&](unsigned int index) { return index < range.vx_end; }));
		}
	}
	const material_range box = base_shape_range(*find_base_shape("box"));
	BOOST_TEST(box.vx_end == 24u);
	BOOST_TEST(box.ind_end == 36u);
	const material_range plane = base_shape_range(*find_base_shape("plane"));
	BOOST_TEST(plane.vx_end == 4u);
	BOOST_TEST(plane.ind_end == 6u);
}

BOOST_AUTO_TEST_CASE(TheShapes_AreUnitSized_AndCentredOnTheOrigin)
{
	const auto extent = [](std::string_view name) {
		const bounds b = bounds_of(bake_shape(*find_base_shape(name)).geometry);
		BOOST_TEST(glm::length(b.min + b.max) < 1e-4f);
		return b.max - b.min;
	};
	const auto near = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; };

	BOOST_TEST(near(extent("box"), { 1.f, 1.f, 1.f }));
	BOOST_TEST(near(extent("plane"), { 1.f, 0.f, 1.f }));
	BOOST_TEST(near(extent("sphere"), { 1.f, 1.f, 1.f }));
	BOOST_TEST(near(extent("cylinder"), { 1.f, 1.f, 1.f }));
	BOOST_TEST(near(extent("capsule_s"), { 1.f, 1.5f, 1.f }));
	BOOST_TEST(near(extent("capsule_m"), { 1.f, 2.f, 1.f }));
	BOOST_TEST(near(extent("capsule_l"), { 1.f, 3.f, 1.f }));
}

// The editor-only source of a mesh never reaches the runtime: the engine does not know its type, and no resource of the
// game's base tree is one (a node points at a *.geom.desc).
BOOST_AUTO_TEST_CASE(PolyMeshDesc_IsNotAnEngineType_AndNoBaseResourceIsOne)
{
	BOOST_TEST(!storage.require<desc::desc_system>().is_registered("poly_mesh_desc"));

	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(SNK_CORE_RES_DIR)) {
		const std::string name = entry.path().filename().string();
		BOOST_TEST(!name.ends_with(".poly.desc"), name);
	}
}

BOOST_AUTO_TEST_SUITE_END()
