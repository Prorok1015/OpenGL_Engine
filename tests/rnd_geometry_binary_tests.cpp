#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include "null_engine_fixture.hpp"
#include "test_temp_dir.hpp"
#include "geom/rnd_geom_manager.h"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// EPIC-79 PR2 (#1068): a geometry_desc reads "vertices" and "indices" from the binary file next to it, checks the data
// before the GPU sees it and follows a change of that file.
namespace {

namespace fs = std::filesystem;

// `triangles` triangles over 3 * `triangles` vertices of one vec3 each, every coordinate `value`.
std::vector<float> positions_of(int triangles, float value = 0.5f)
{
	return std::vector<float>(static_cast<std::size_t>(9 * triangles), value);
}

std::vector<std::uint32_t> indices_of(int triangles)
{
	std::vector<std::uint32_t> out;
	for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(3 * triangles); ++i) {
		out.push_back(i);
	}
	return out;
}

// The desc of a binary geometry: `layout` and the two ranges; `extra` is spliced in as more members.
std::string desc_text(std::span<const res::binary_range> ranges, std::string_view layout = R"({"position":"vec3"})",
	std::string_view extra = "")
{
	std::string text = R"({"__type":"geometry_desc","__version":2,"layout":)" + std::string(layout);
	text += ",\"vertices\":" + boost::json::serialize(res::to_json(ranges[0]));
	text += ",\"indices\":" + boost::json::serialize(res::to_json(ranges[1]));
	text += std::string(extra) + "}";
	return text;
}

struct geometry_binary_fixture : test_util::null_engine_fixture
{
	res::resource_system& root() { return storage.require<res::resource_system>(); }
	rnd::driver::driver_interface* gpu() { return storage.require<rnd::render_system>().get_driver(); }

	static res::tag binary_of(const res::tag& desc)
	{
		const auto binary = res::desc_binary::binary_tag(desc);
		BOOST_REQUIRE(binary.has_value());
		return *binary;
	}

	// The binary file of `desc` and `desc` itself, through memory://, without a signal.
	std::vector<res::binary_range> put(const res::tag& desc, std::span<const std::byte> vertices,
		const std::vector<std::uint32_t>& indices, std::string_view vertex_layout = "f32x3",
		std::string_view layout = R"({"position":"vec3"})", std::string_view extra = "")
	{
		const std::array<res::binary_chunk, 2> chunks{ res::binary_chunk{ vertices, vertex_layout },
			res::binary_chunk{ std::as_bytes(std::span(indices)), "u32" } };
		std::vector<res::binary_range> ranges;
		std::string error;
		const auto file = res::desc_binary::encode(chunks, ranges, error);
		BOOST_REQUIRE_MESSAGE(file.has_value(), error);
		BOOST_REQUIRE(root().store(binary_of(desc), *file));
		BOOST_REQUIRE(root().store(desc, test_util::to_bytes(desc_text(ranges, layout, extra))));
		test_util::pump(tasks);
		return ranges;
	}

	std::vector<res::binary_range> put(const res::tag& desc, const std::vector<float>& positions,
		const std::vector<std::uint32_t>& indices)
	{
		return put(desc, std::as_bytes(std::span(positions)), indices);
	}

	template<class T>
	res::res_handle<T> load(const res::tag& tag)
	{
		auto handle = root().require_sync<T>(tag);
		test_util::pump(tasks);
		return handle;
	}

	rnd::driver::vertex_array_interface* frame(rnd::geom_manager& geometry, const res::tag& tag)
	{
		test_util::pump(tasks);
		geometry.begin_frame();
		return geometry.require_geometry(tag);
	}

	rnd::driver::vertex_array_interface* build(rnd::geom_manager& geometry, const res::tag& tag)
	{
		rnd::driver::vertex_array_interface* built = nullptr;
		for (int attempt = 0; attempt < 16 && !(built = frame(geometry, tag)); ++attempt) {}
		return built;
	}
};

std::vector<float> floats_of(const std::vector<std::byte>& bytes)
{
	std::vector<float> out;
	BOOST_REQUIRE(res::copy_binary_values(bytes, out));
	return out;
}

bool mentions(const std::string& text, std::string_view part)
{
	return text.find(part) != std::string::npos;
}

}

BOOST_FIXTURE_TEST_SUITE(RndGeometryBinaryTests, geometry_binary_fixture)

BOOST_AUTO_TEST_CASE(AGeometrySavedWithItsBinaryFile_LoadsItsVerticesAndIndices)
{
	// A project layer on disk: the save goes through a staged file and a commit (res::desc_binary::save).
	test_util::temp_dir dir{ "rnd_geometry_binary" };
	fs::create_directories(dir.path() / "a");
	root().registrate_resolver<res::vfs_resolver>("geombin", std::vector<fs::path>{ dir.path() });
	const res::tag tag{ "geombin://a/robot.geom.desc" };
	const auto positions = positions_of(2);
	const auto indices = indices_of(2);
	const std::array<res::binary_chunk, 2> chunks{ res::binary_chunk{ std::as_bytes(std::span(positions)), "f32x3" },
		res::binary_chunk{ std::as_bytes(std::span(indices)), "u32" } };
	const auto written = res::desc_binary::save(root(), tag, chunks, [&](std::span<const res::binary_range> ranges) {
		return root().store_sync(tag, test_util::to_bytes(desc_text(ranges)));
	});
	BOOST_REQUIRE_MESSAGE(written, written.error);
	BOOST_TEST(fs::exists(dir.path() / "a" / "robot.geom.bin"));
	BOOST_TEST(!fs::exists(dir.path() / "a" / "robot.geom.bin.new"));

	{
		const auto geometry = load<rnd::geometry_desc>(tag);
		BOOST_REQUIRE_MESSAGE(geometry.is_ready(), geometry.error_text());
		BOOST_TEST((floats_of(geometry->vertices) == positions));
		BOOST_TEST((std::vector<std::uint32_t>(geometry->indices.begin(), geometry->indices.end()) == indices));
		BOOST_TEST(geometry->layout.get_stride() == 12u);
	}
	root().unregistrate_resolver("geombin");
}

BOOST_AUTO_TEST_CASE(TheArrayFormat_StillLoads_AndIsWrittenBack)
{
	const res::tag tag{ "memory://geometry_binary/arrays.geom.desc" };
	std::string text = R"({"__type":"geometry_desc","__version":1,"layout":{"position":"vec3"},"indices":[0,1,2],"vertices":[)";
	for (int i = 0; i < 36; ++i) {
		text += i == 0 ? "0" : ",0";
	}
	text += "]}";
	BOOST_REQUIRE(root().store(tag, test_util::to_bytes(text)));
	test_util::pump(tasks);

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(geometry.is_ready(), geometry.error_text());
	BOOST_TEST(geometry->vertices.size() == 36u);
	BOOST_TEST(geometry->indices.size() == 3u);
	boost::json::object written;
	BOOST_CHECK_NO_THROW(geometry->serialize(written));
	BOOST_TEST(written.at("vertices").is_array());
}

BOOST_AUTO_TEST_CASE(ANanPosition_IsRefused_NamingVertices)
{
	const res::tag tag{ "memory://geometry_binary/nan.geom.desc" };
	auto positions = positions_of(1);
	positions[4] = std::numeric_limits<float>::quiet_NaN();
	put(tag, positions, indices_of(1));

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE(geometry.has_error());
	BOOST_TEST(mentions(geometry.error_text(), "nan.geom.desc"));
	BOOST_TEST(mentions(geometry.error_text(), "/vertices"));
	BOOST_TEST(mentions(geometry.error_text(), "vertex 1 has a 'position' that is not finite"));
}

BOOST_AUTO_TEST_CASE(AnIndexPastTheVertices_IsRefused_NamingIndices)
{
	const res::tag tag{ "memory://geometry_binary/index.geom.desc" };
	put(tag, positions_of(1), std::vector<std::uint32_t>{ 0, 1, 3 });

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE(geometry.has_error());
	BOOST_TEST(mentions(geometry.error_text(), "/indices"));
	BOOST_TEST(mentions(geometry.error_text(), "index 2 is 3, past the 3 vertices"));
}

BOOST_AUTO_TEST_CASE(IndicesThatAreNotWholeTriangles_AreRefused)
{
	const res::tag tag{ "memory://geometry_binary/partial.geom.desc" };
	put(tag, positions_of(1), std::vector<std::uint32_t>{ 0, 1 });

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE(geometry.has_error());
	BOOST_TEST(mentions(geometry.error_text(), "/indices"));
	BOOST_TEST(mentions(geometry.error_text(), "2 indices are not whole triangles"));
}

BOOST_AUTO_TEST_CASE(VertexBytesOffTheStride_AreRefused)
{
	const res::tag tag{ "memory://geometry_binary/stride.geom.desc" };
	const std::vector<std::byte> bytes(37, std::byte{ 0 });
	put(tag, bytes, indices_of(1), "u8");

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE(geometry.has_error());
	BOOST_TEST(mentions(geometry.error_text(), "/vertices"));
	BOOST_TEST(mentions(geometry.error_text(), "37 bytes are not a whole number of 12-byte vertices"));
}

BOOST_AUTO_TEST_CASE(AVertexRangeOfAnotherElement_IsRefused_NamingItsLayout)
{
	const res::tag tag{ "memory://geometry_binary/layout.geom.desc" };
	// 72 bytes are whole f32x2 elements, but a vertex of the layout is 12 bytes.
	const auto positions = positions_of(2);
	put(tag, std::as_bytes(std::span(positions)), indices_of(2), "f32x2");

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE(geometry.has_error());
	BOOST_TEST(mentions(geometry.error_text(), "/vertices/layout"));
}

BOOST_AUTO_TEST_CASE(AMissingBinaryFile_FailsTheLoad)
{
	const res::tag tag{ "memory://geometry_binary/missing.geom.desc" };
	put(tag, positions_of(1), indices_of(1));
	BOOST_REQUIRE(root().erase(binary_of(tag)));
	test_util::pump(tasks);

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE(geometry.has_error());
	BOOST_TEST(mentions(geometry.error_text(), "missing.geom.bin: the file does not exist"));
}

BOOST_AUTO_TEST_CASE(StoredBounds_AreBuiltAgainFromTheData)
{
	const res::tag tag{ "memory://geometry_binary/bounds.geom.desc" };
	// position vec3 + uv vec2: the uv of the three vertices are (0,0), (1,0), (0,2).
	const std::vector<float> vertices{ 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 2 };
	put(tag, std::as_bytes(std::span(vertices)), indices_of(1), "u8", R"({"position":"vec3","uv":"vec2"})",
		R"(,"bounds":[[[{"x":7,"y":7},{"x":9,"y":9}],0]])");

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(geometry.is_ready(), geometry.error_text());
	BOOST_REQUIRE(geometry->bounds.size() == 1u);
	BOOST_TEST((geometry->bounds[0].first == ds::bbox{ { 0.0f, 0.0f }, { 1.0f, 2.0f } }));
}

BOOST_AUTO_TEST_CASE(TheBinaryFile_IsADependency_AndItsChangeReloadsTheGeometry)
{
	const res::tag tag{ "memory://geometry_binary/dep.geom.desc" };
	put(tag, positions_of(1), indices_of(1));
	const std::size_t edges = root().dependency_edge_count();
	const auto before = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(before.is_ready(), before.error_text());
	BOOST_TEST(root().dependency_edge_count() == edges + 1u);

	// A save of new data: both files change, only the binary file is signalled.
	put(tag, positions_of(2, 0.25f), indices_of(2));
	root().signal_changed(binary_of(tag));
	test_util::pump(tasks);
	const auto after = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(after.is_ready(), after.error_text());
	BOOST_TEST((after.get_control_block() != before.get_control_block()));
	BOOST_TEST((floats_of(after->vertices) == positions_of(2, 0.25f)));
}

BOOST_AUTO_TEST_CASE(AChangedBinaryFile_IsDrawn_AndABadOneKeepsThePreviousGeometry)
{
	const res::tag tag{ "memory://geometry_binary/reload.geom.desc" };
	put(tag, positions_of(1), indices_of(1));
	rnd::geom_manager geometry(gpu(), root());
	rnd::driver::vertex_array_interface* const first = build(geometry, tag);
	BOOST_REQUIRE(first != nullptr);
	BOOST_TEST(!geometry.draw_fits(tag, 6, 0, 0));

	std::mutex said_mutex;
	std::size_t warned = 0;
	const auto sink = engine::add_log_sink([&](std::string_view category, engine::log_level, std::string_view message) {
		if (category != "render/geometry" || message.find("reload.geom.desc") == std::string_view::npos) return;
		std::lock_guard lock(said_mutex);
		warned += message.find("failed to load") != std::string_view::npos ? 1u : 0u;
	});

	// The binary file alone changes: its hashes no longer match the desc, the previous geometry stays.
	const auto other = positions_of(1, 0.75f);
	std::vector<res::binary_range> ranges;
	std::string error;
	const std::vector<std::uint32_t> indices = indices_of(1);
	const std::array<res::binary_chunk, 2> chunks{ res::binary_chunk{ std::as_bytes(std::span(other)), "f32x3" },
		res::binary_chunk{ std::as_bytes(std::span(indices)), "u32" } };
	const auto mismatched = res::desc_binary::encode(chunks, ranges, error);
	BOOST_REQUIRE(mismatched.has_value());
	BOOST_REQUIRE(root().store(binary_of(tag), *mismatched));
	test_util::pump(tasks);
	root().signal_changed(binary_of(tag));
	for (int i = 0; i < 16; ++i) {
		BOOST_TEST_CONTEXT("frame " << i) { BOOST_TEST(frame(geometry, tag) == first); }
	}
	{
		std::lock_guard lock(said_mutex);
		BOOST_TEST(warned == 1u);
	}

	// The next good save is drawn.
	put(tag, positions_of(2), indices_of(2));
	root().signal_changed(binary_of(tag));
	for (int i = 0; i < 16 && geometry.require_geometry(tag) == first; ++i) {
		BOOST_REQUIRE(frame(geometry, tag) != nullptr);
	}
	BOOST_TEST(geometry.draw_fits(tag, 6, 0, 0));
}

BOOST_AUTO_TEST_CASE(ABinaryGeometryReadAsAPlainDesc_HasNoData_AndIsNeverWritten)
{
	const res::tag tag{ "memory://geometry_binary/plain.geom.desc" };
	put(tag, positions_of(1), indices_of(1));

	const auto plain = load<desc::desc_base>(tag);
	BOOST_REQUIRE_MESSAGE(plain.is_ready(), plain.error_text());
	const auto* geometry = dynamic_cast<const rnd::geometry_desc*>(&*plain);
	BOOST_REQUIRE(geometry != nullptr);
	BOOST_TEST(geometry->vertices.empty());
	BOOST_TEST(geometry->indices.empty());
	boost::json::object written;
	BOOST_CHECK_THROW(geometry->serialize(written), std::runtime_error);
	// A copy does not lose the mark.
	const rnd::geometry_desc copy = *geometry;
	BOOST_CHECK_THROW(copy.serialize(written), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ABinaryGeometryLoaded_IsNotWrittenBackAsArrays)
{
	const res::tag tag{ "memory://geometry_binary/loaded.geom.desc" };
	put(tag, positions_of(1), indices_of(1));

	const auto geometry = load<rnd::geometry_desc>(tag);
	BOOST_REQUIRE_MESSAGE(geometry.is_ready(), geometry.error_text());
	boost::json::object written;
	BOOST_CHECK_THROW(geometry->serialize(written), std::runtime_error);
	rnd::geometry_desc copy;
	geometry->copy_to(copy);
	BOOST_TEST(copy.vertices.size() == 36u);
	BOOST_CHECK_THROW(copy.serialize(written), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ALayoutWithoutVertices_IsNeverWritten_WhateverTheTableSays)
{
	// Arrays only: no binary state.
	rnd::geometry_desc geometry;
	geometry.layout = rnd::driver::BufferLayout{ { rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" } };
	geometry.indices = { 0, 1, 2 };
	boost::json::object written;
	BOOST_CHECK_THROW(geometry.serialize(written), std::runtime_error);

	// No layout and no data (core/res/base_geometry.desc) is still written.
	rnd::geometry_desc empty;
	BOOST_CHECK_NO_THROW(empty.serialize(written));
}

BOOST_AUTO_TEST_CASE(TheBinaryState_IsAMember_CopiedByCopyAndAssignment)
{
	// Ranges with no layout: the recorded state is all the geometry holds.
	const auto positions = positions_of(1);
	const auto indices = indices_of(1);
	const std::array<res::binary_chunk, 2> chunks{ res::binary_chunk{ std::as_bytes(std::span(positions)), "f32x3" },
		res::binary_chunk{ std::as_bytes(std::span(indices)), "u32" } };
	std::vector<res::binary_range> ranges;
	std::string error;
	BOOST_REQUIRE_MESSAGE(res::desc_binary::encode(chunks, ranges, error).has_value(), error);
	const boost::json::object text = boost::json::parse(desc_text(ranges, "{}")).as_object();

	rnd::geometry_desc source;
	source.deserialize(storage.require<desc::desc_system>(), text);
	BOOST_TEST((source.binary.state == rnd::detail::geometry_binary_state::not_loaded));
	BOOST_REQUIRE(source.binary.ranges.vertices.has_value());
	BOOST_TEST(source.binary.ranges.vertices->sha256 == ranges[0].sha256);

	const rnd::geometry_desc copied = source;
	BOOST_TEST((copied.binary.state == rnd::detail::geometry_binary_state::not_loaded));
	BOOST_REQUIRE(copied.binary.ranges.indices.has_value());
	BOOST_TEST(copied.binary.ranges.indices->offset == ranges[1].offset);
	boost::json::object written;
	BOOST_CHECK_THROW(copied.serialize(written), std::runtime_error);

	rnd::geometry_desc assigned;
	BOOST_CHECK_NO_THROW(assigned.serialize(written));
	assigned = source;
	BOOST_TEST((assigned.binary.state == rnd::detail::geometry_binary_state::not_loaded));
	BOOST_CHECK_THROW(assigned.serialize(written), std::runtime_error);

	// Arrays read over it drop the state: the geometry is written as arrays again.
	assigned.deserialize(storage.require<desc::desc_system>(),
		boost::json::parse(R"({"layout":{},"vertices":[],"indices":[]})").as_object());
	BOOST_TEST((assigned.binary.state == rnd::detail::geometry_binary_state::none));
	BOOST_TEST((source.binary.state == rnd::detail::geometry_binary_state::not_loaded));
}

BOOST_AUTO_TEST_CASE(AChildOfABinaryGeometry_IsWrittenWhole_KeepingItsParent)
{
	const res::tag parent_tag{ "memory://geometry_binary/parent.geom.desc" };
	put(parent_tag, positions_of(1), indices_of(1));
	auto& descs = storage.require<desc::desc_system>();
	const auto instance = descs.create_instance("geometry_desc", res::tag{ "memory://geometry_binary/child.geom.desc" });
	auto* child = dynamic_cast<rnd::geometry_desc*>(instance.get());
	BOOST_REQUIRE(child != nullptr);
	child->layout = rnd::driver::BufferLayout{ { rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position" } };
	child->indices = { 0, 1, 2 };
	child->vertices.assign(36, std::byte{ 0 });
	child->set_parent(parent_tag);

	// The parent refuses to be written: no fields to compare with, the child is written whole and still loads on it.
	boost::json::object body;
	child->serialize(body);
	BOOST_REQUIRE_NO_THROW(descs.reduce_to_parent(*child, body));
	BOOST_TEST(body.contains("__parent"));
	BOOST_TEST(body.at("vertices").is_array());
	BOOST_TEST(body.at("vertices").as_array().size() == 36u);
	BOOST_TEST(body.at("indices").as_array().size() == 3u);
}

namespace {

// One triangle as arrays (3 vertices of one vec3, one byte a number) with its bounds at `bounds`, if given.
boost::json::object array_geometry(int vertex_byte, std::string_view bounds = "")
{
	std::string text = R"({"layout":{"position":"vec3"},"indices":[0,1,2],"vertices":[)";
	for (int i = 0; i < 36; ++i) text += (i ? "," : "") + std::to_string(vertex_byte);
	text += "]";
	if (!bounds.empty()) text += ",\"bounds\":" + std::string(bounds);
	return boost::json::parse(text + "}").as_object();
}

const ds::bbox parent_box{ { 7.0f, 7.0f }, { 9.0f, 9.0f } };
constexpr std::string_view parent_bounds = R"([[[{"x":7,"y":7},{"x":9,"y":9}],0]])";

// A child as the adapter reads it (#1180): the parent copied in first, then the child's own JSON over it.
rnd::geometry_desc child_of(desc::desc_system& descs, const boost::json::object& child_json)
{
	rnd::geometry_desc parent;
	parent.deserialize(descs, array_geometry(1, parent_bounds));
	rnd::geometry_desc child;
	parent.copy_to(child);
	child.deserialize(descs, child_json);
	return child;
}

}

BOOST_AUTO_TEST_CASE(AChildWithItsOwnArraysAndNoBounds_DoesNotKeepTheBoundsOfItsParent)
{
	const rnd::geometry_desc child = child_of(storage.require<desc::desc_system>(), array_geometry(2));
	BOOST_TEST(child.bounds.empty());
	BOOST_TEST(child.rtree.query(parent_box).empty());
}

BOOST_AUTO_TEST_CASE(AChildWithItsOwnBounds_HasOnlyThoseInItsTree)
{
	const rnd::geometry_desc child = child_of(storage.require<desc::desc_system>(),
		array_geometry(2, R"([[[{"x":0,"y":0},{"x":1,"y":1}],0]])"));
	BOOST_REQUIRE(child.bounds.size() == 1u);
	BOOST_TEST(child.rtree.query(parent_box).empty());
	BOOST_TEST(child.rtree.query(ds::bbox{ { 0.0f, 0.0f }, { 1.0f, 1.0f } }).size() == 1u);
}

BOOST_AUTO_TEST_CASE(AChildWithItsOwnIndicesAlone_DoesNotKeepTheBoundsOfItsParent)
{
	const rnd::geometry_desc child = child_of(storage.require<desc::desc_system>(),
		boost::json::parse(R"({"layout":{"position":"vec3"},"indices":[2,1,0],"vertices":[]})").as_object());
	BOOST_TEST(child.vertices.size() == 36u);
	BOOST_TEST((child.indices == std::vector<unsigned int>{ 2, 1, 0 }));
	BOOST_TEST(child.bounds.empty());
	BOOST_TEST(child.rtree.query(parent_box).empty());
}

BOOST_AUTO_TEST_CASE(AChildWithBoundsAlone_HasItsOwnBoundsOverTheDataOfItsParent)
{
	const rnd::geometry_desc child = child_of(storage.require<desc::desc_system>(),
		boost::json::parse(R"({"layout":{"position":"vec3"},"indices":[],"vertices":[],"bounds":[[[{"x":0,"y":0},{"x":1,"y":1}],0]]})").as_object());
	BOOST_TEST(child.vertices.size() == 36u);
	BOOST_REQUIRE(child.bounds.size() == 1u);
	BOOST_TEST(child.rtree.query(parent_box).empty());
	BOOST_TEST(child.rtree.query(ds::bbox{ { 0.0f, 0.0f }, { 1.0f, 1.0f } }).size() == 1u);
}

BOOST_AUTO_TEST_CASE(AChildWithoutDataOfItsOwn_KeepsTheDataAndBoundsOfItsParent)
{
	const rnd::geometry_desc child = child_of(storage.require<desc::desc_system>(),
		boost::json::parse(R"({"layout":{"position":"vec3"},"indices":[],"vertices":[]})").as_object());
	BOOST_TEST(child.vertices.size() == 36u);
	BOOST_REQUIRE(child.bounds.size() == 1u);
	BOOST_TEST(child.rtree.query(parent_box).size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
