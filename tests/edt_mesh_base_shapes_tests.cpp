#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "null_engine_fixture.hpp"
#include "edt_mesh_base_shapes.h"
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_files.h>
#include <snk/model_codec/res_tag.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
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

std::vector<std::byte> read_bytes(const fs::path& path)
{
	const std::string text = read_text(path);
	std::vector<std::byte> out(text.size());
	if (!text.empty()) {
		std::memcpy(out.data(), text.data(), text.size());
	}
	return out;
}

res::tag tag_of(const base_shape& shape)
{
	return res::tag{ base_shape_path(shape) };
}

// The desc without what follows from the bytes of the binary file: the SHA-256 of each range and where it starts.
boost::json::object without_hashes(boost::json::object desc)
{
	for (const std::string_view key : { "vertices", "indices" }) {
		if (boost::json::value* range = desc.if_contains(key); range && range->is_object()) {
			range->as_object().erase("sha256");
			range->as_object().erase("offset");
		}
	}
	return desc;
}

// The float arithmetic of sin, cos and acos is the C runtime's: another compiler or runtime may differ in the last bit of a
// vertex, and so in the SHA-256 of the binary file. So a pair is compared as data: the desc's fields exactly but the hashes
// and offsets of its ranges, both pairs decoded as the loader reads them, the indices exactly and the vertices float by float
// with a tolerance. Empty: the committed pair is what the generator makes; else the first difference.
std::string difference(const res::tag& tag, const base_shape_pair& expected, const base_shape_pair& committed)
{
	constexpr float tolerance = 1e-6f;
	boost::json::value expected_value;
	boost::json::value committed_value;
	try {
		expected_value = boost::json::parse(expected.desc);
		committed_value = boost::json::parse(committed.desc);
	} catch (const std::exception& error) {
		return std::string("the desc is not JSON: ") + error.what();
	}
	if (!expected_value.is_object() || !committed_value.is_object()) {
		return "the desc is not an object";
	}
	const boost::json::object& expected_desc = expected_value.as_object();
	const boost::json::object& committed_desc = committed_value.as_object();
	const boost::json::object want_fields = without_hashes(expected_desc);
	const boost::json::object have_fields = without_hashes(committed_desc);
	if (want_fields.size() != have_fields.size()) {
		return "the desc has other keys";
	}
	for (const auto& member : want_fields) {
		const boost::json::value* other = have_fields.if_contains(member.key());
		if (!other) {
			return "'" + std::string(member.key()) + "' is missing";
		}
		if (member.value() != *other) {
			return "'" + std::string(member.key()) + "' differs";
		}
	}

	rnd::geometry_desc want;
	rnd::geometry_desc have;
	if (const std::string problem = rnd::decode_geometry_files(tag, expected_desc, expected.binary, want); !problem.empty()) {
		return "the generator's pair does not decode: " + problem;
	}
	if (const std::string problem = rnd::decode_geometry_files(tag, committed_desc, committed.binary, have); !problem.empty()) {
		return "the pair does not decode: " + problem;
	}
	if (want.indices != have.indices) {
		return "the indices differ";
	}
	if (want.vertices.size() != have.vertices.size()) {
		return "the number of vertex bytes differs";
	}
	for (std::size_t i = 0; i + sizeof(float) <= want.vertices.size(); i += sizeof(float)) {
		float a = 0.f;
		float b = 0.f;
		std::memcpy(&a, want.vertices.data() + i, sizeof(float));
		std::memcpy(&b, have.vertices.data() + i, sizeof(float));
		if (!(std::abs(a - b) <= tolerance)) {
			return "float " + std::to_string(i / sizeof(float)) + " of the vertices differs: " + std::to_string(a) + " against " + std::to_string(b);
		}
	}
	return {};
}

// The pair of `pair`'s data with `change` made to it, encoded again as the generator encodes it.
base_shape_pair changed(const res::tag& tag, const base_shape_pair& pair, const std::function<void(rnd::geometry_desc&)>& change)
{
	const boost::json::object desc = boost::json::parse(pair.desc).as_object();
	rnd::geometry_desc geometry;
	const std::string problem = rnd::decode_geometry_files(tag, desc, pair.binary, geometry);
	BOOST_REQUIRE_MESSAGE(problem.empty(), problem);
	change(geometry);
	rnd::geometry_file_options options;
	options.bounds = desc.contains("bounds");
	const rnd::geometry_files files = rnd::encode_geometry_files(tag, geometry, options);
	BOOST_REQUIRE_MESSAGE(static_cast<bool>(files), files.error);
	return { std::string(reinterpret_cast<const char*>(files.desc.data()), files.desc.size()), files.binary };
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

// A mismatch writes the generator's pair to generated_shapes/ next to the test binary: the Linux Clang job of CI uploads that
// folder as the artifact generated-base-shapes, and its files (desc and .bin) go into core/res/shapes unchanged (no one
// builds locally).
BOOST_AUTO_TEST_CASE(TheCommittedFiles_AreWhatTheBakeWrites)
{
	for (const base_shape& shape : base_shapes()) {
		BOOST_TEST_CONTEXT(shape.name) {
			const base_shape_pair expected = base_shape_files(shape);
			const fs::path desc = fs::path(SNK_CORE_RES_DIR) / "shapes" / (std::string(shape.name) + ".geom.desc");
			const fs::path binary = fs::path(SNK_CORE_RES_DIR) / "shapes" / (std::string(shape.name) + ".geom.bin");
			const base_shape_pair committed{ read_text(desc), read_bytes(binary) };
			const std::string found = difference(tag_of(shape), expected, committed);
			if (!found.empty()) {
				const fs::path folder = "generated_shapes";
				fs::create_directories(folder);
				std::ofstream(folder / desc.filename(), std::ios::binary) << expected.desc;
				std::ofstream(folder / binary.filename(), std::ios::binary)
					.write(reinterpret_cast<const char*>(expected.binary.data()), static_cast<std::streamsize>(expected.binary.size()));
				BOOST_ERROR("core/res/shapes/" + desc.filename().string() + " and " + binary.filename().string()
					+ " are missing or differ from the generator (" + found
					+ "): take both files from the generated-base-shapes artifact of CI");
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(TheComparison_AcceptsAFloatOffByTheTolerance_AndRefusesAMovedVertexAnotherIndexOrAnotherField)
{
	const base_shape& plane = *find_base_shape("plane");
	const res::tag tag = tag_of(plane);
	const base_shape_pair pair = base_shape_files(plane);
	BOOST_TEST(difference(tag, pair, pair).empty());

	// The first vertex's x is the float with the bytes 00 00 00 BF (-0.5): one bit more is a change of 6e-8 (and another
	// SHA-256), BE in the high byte is -0.125.
	const base_shape_pair one_bit = changed(tag, pair, [](rnd::geometry_desc& g) { g.vertices[0] = std::byte{ 1 }; });
	BOOST_TEST((one_bit.binary != pair.binary));
	BOOST_TEST(difference(tag, pair, one_bit).empty());
	BOOST_TEST(!difference(tag, pair, changed(tag, pair, [](rnd::geometry_desc& g) { g.vertices[3] = std::byte{ 0xBE }; })).empty());
	BOOST_TEST(!difference(tag, pair, changed(tag, pair, [](rnd::geometry_desc& g) { g.indices[2] = 3; })).empty());

	boost::json::object other_field = boost::json::parse(pair.desc).as_object();
	other_field["__guid"] = "00000000-0000-4000-8000-000000000001";
	BOOST_TEST(!difference(tag, pair, { boost::json::serialize(other_field), pair.binary }).empty());
	BOOST_TEST(!difference(tag, pair, { "[]", pair.binary }).empty());
	BOOST_TEST(!difference(tag, pair, { pair.desc, {} }).empty());
}

BOOST_AUTO_TEST_CASE(TheFileVersion_IsTheEnginesGeometryVersion_AndTheDescIsPrettyWithRanges)
{
	BOOST_TEST(base_shape_file_version == storage.require<desc::desc_system>().current_version("geometry_desc"));
	for (const base_shape& shape : base_shapes()) {
		BOOST_TEST_CONTEXT(shape.name) {
			const base_shape_pair pair = base_shape_files(shape);
			BOOST_TEST(pair.desc.starts_with("{\n    \"__type\" : \"geometry_desc\",\n    \"__version\" : 2,\n"));
			BOOST_TEST(pair.desc.back() == '\n');
			BOOST_TEST(pair.desc.find("__parent") == std::string::npos);
			const boost::json::object desc = boost::json::parse(pair.desc).as_object();
			BOOST_TEST(desc.at("vertices").is_object());
			BOOST_TEST(desc.at("indices").is_object());
			BOOST_TEST(pair.binary.size() > 8u);
			// Two runs give the same bytes: nothing of the time or of chance goes into a pair.
			const base_shape_pair again = base_shape_files(shape);
			BOOST_TEST(again.desc == pair.desc);
			BOOST_TEST((again.binary == pair.binary));
		}
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
