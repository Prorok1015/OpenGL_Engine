#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "null_engine_fixture.hpp"
#include "edt_mesh_base_shapes.h"
#include <snk/desc/desc_system.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace edt::mesh;

namespace fs = std::filesystem;

constexpr std::size_t vertex_stride = 14 * sizeof(float);

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::vector<std::byte> bytes_of(const boost::json::array& numbers)
{
	std::vector<std::byte> out;
	out.reserve(numbers.size());
	for (const boost::json::value& number : numbers) {
		out.push_back(static_cast<std::byte>(boost::json::value_to<unsigned>(number)));
	}
	return out;
}

// The float arithmetic of sin, cos and acos is the C runtime's: another compiler or runtime may differ in the last bit of a
// vertex. So the file is compared as data: everything but the vertices exactly, the vertices float by float with a tolerance.
// Empty: the file is what the generator writes; else the first difference.
std::string difference(const std::string& expected_text, const std::string& committed_text)
{
	constexpr float tolerance = 1e-6f;
	boost::json::value expected_value;
	boost::json::value committed_value;
	try {
		expected_value = boost::json::parse(expected_text);
		committed_value = boost::json::parse(committed_text);
	} catch (const std::exception& error) {
		return std::string("the file is not JSON: ") + error.what();
	}
	if (!expected_value.is_object() || !committed_value.is_object()) {
		return "the file is not an object";
	}
	const boost::json::object& expected = expected_value.as_object();
	const boost::json::object& committed = committed_value.as_object();
	if (expected.size() != committed.size()) {
		return "the file has other keys";
	}
	for (const auto& member : expected) {
		const std::string_view key = member.key();
		const boost::json::value& value = member.value();
		const boost::json::value* other = committed.if_contains(key);
		if (!other) {
			return "'" + std::string(key) + "' is missing";
		}
		if (key != "vertices") {
			if (value != *other) {
				return "'" + std::string(key) + "' differs";
			}
			continue;
		}
		if (!value.is_array() || !other->is_array() || value.as_array().size() != other->as_array().size()) {
			return "the number of vertex bytes differs";
		}
		const std::vector<std::byte> want = bytes_of(value.as_array());
		const std::vector<std::byte> have = bytes_of(other->as_array());
		for (std::size_t i = 0; i + sizeof(float) <= want.size(); i += sizeof(float)) {
			float a = 0.f;
			float b = 0.f;
			std::memcpy(&a, want.data() + i, sizeof(float));
			std::memcpy(&b, have.data() + i, sizeof(float));
			if (!(std::abs(a - b) <= tolerance)) {
				return "float " + std::to_string(i / sizeof(float)) + " of the vertices differs: " + std::to_string(a) + " against " + std::to_string(b);
			}
		}
	}
	return {};
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
			const std::string found = difference(expected, read_text(committed));
			if (!found.empty()) {
				const fs::path generated = fs::path("generated_shapes") / committed.filename();
				fs::create_directories(generated.parent_path());
				std::ofstream(generated, std::ios::binary) << expected;
				BOOST_ERROR("core/res/shapes/" + committed.filename().string() + " is missing or differs from the generator (" + found
					+ "): take the file from the generated_shapes artifact");
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(TheComparison_AcceptsAFloatOffByTheTolerance_AndRefusesAMovedVertexOrAnotherIndex)
{
	const std::string text = base_shape_file_text(*find_base_shape("plane"));
	BOOST_TEST(difference(text, text).empty());

	// The first vertex's x is the float with the bytes 00 00 00 BF (-0.5): one bit more is a change of 6e-8, 0xBE80 in the high
	// bytes is -0.25.
	const auto changed = [&](const char* array, std::size_t index, unsigned value) {
		boost::json::value file = boost::json::parse(text);
		file.as_object()[array].as_array()[index] = value;
		return boost::json::serialize(file);
	};
	BOOST_TEST(changed("vertices", 0, 1) != text);
	BOOST_TEST(difference(text, changed("vertices", 0, 1)).empty());
	BOOST_TEST(!difference(text, changed("vertices", 3, 190)).empty());
	BOOST_TEST(!difference(text, changed("indices", 2, 3)).empty());
	BOOST_TEST(!difference(text, "[]").empty());
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
