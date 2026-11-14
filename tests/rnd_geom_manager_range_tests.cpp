#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "geom/rnd_geom_manager.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// #927: what geom_manager::draw_fits answers for a geometry written by hand, so no model importer is needed.
namespace {

struct geom_range_fixture : test_util::null_engine_fixture
{
	// A geometry desc under memory://, built by `geometry` and returned once its buffers exist.
	res::tag build(rnd::geom_manager& geometry, std::string_view name, std::string_view layout, std::size_t vertex_bytes, std::string_view indices)
	{
		std::string text = R"json({"__type":"geometry_desc","layout":)json";
		text += layout;
		text += R"json(,"indices":)json";
		text += indices;
		text += R"json(,"vertices":[)json";
		for (std::size_t i = 0; i < vertex_bytes; ++i) {
			text += i == 0 ? "0" : ",0";
		}
		text += "]}";

		const res::tag tag{ std::string{ "memory://geom_range/" } + std::string{ name } };
		storage.require<res::resource_system>().store(tag, test_util::to_bytes(text));
		test_util::pump(tasks);
		for (int attempt = 0; attempt < 16 && !geometry.require_geometry(tag); ++attempt) {
			test_util::pump(tasks);
		}
		BOOST_REQUIRE(geometry.find_geometry(tag) != nullptr);
		return tag;
	}

	rnd::driver::driver_interface* gpu() { return storage.require<rnd::render_system>().get_driver(); }
};

constexpr std::uint32_t k_max = std::numeric_limits<std::uint32_t>::max();

}

BOOST_FIXTURE_TEST_SUITE(RndGeomManagerRange, geom_range_fixture)

// Two meshes in one geometry of 7 vertices: a quad (vertices 0..3, indices 0..5) and a triangle (vertices 4..6, indices 6..8).
BOOST_AUTO_TEST_CASE(MeshesOfOneGeometry_FitUpToTheLastVertexAndIndex)
{
	rnd::geom_manager geometry(gpu(), storage.require<res::resource_system>());
	const res::tag tag = build(geometry, "two_meshes.desc", R"json({"position":"vec3"})json", 7 * 12, "[0,1,2,2,1,3,0,1,2]");

	// draw_fits(tag, count, base_vertex, base_index)
	BOOST_TEST(geometry.draw_fits(tag, 6, 0, 0));
	// base_vertex + max_index == vertex_count - 1: the last vertex is drawn
	BOOST_TEST(geometry.draw_fits(tag, 3, 4, 6));
	BOOST_TEST(geometry.draw_fits(tag, 6, 3, 0));
	// base_vertex + max_index == vertex_count: one past the last vertex
	BOOST_TEST(!geometry.draw_fits(tag, 3, 5, 6));
	BOOST_TEST(!geometry.draw_fits(tag, 6, 4, 0));

	BOOST_TEST(geometry.draw_fits(tag, 9, 0, 0));
	BOOST_TEST(geometry.draw_fits(tag, 1, 0, 8));
	BOOST_TEST(!geometry.draw_fits(tag, 10, 0, 0));
	BOOST_TEST(!geometry.draw_fits(tag, 4, 0, 6));
	BOOST_TEST(!geometry.draw_fits(tag, 1, 0, 9));
	// the end of the range does not wrap around 32 bits
	BOOST_TEST(!geometry.draw_fits(tag, k_max, 0, k_max));
	BOOST_TEST(!geometry.draw_fits(tag, 2, 0, k_max));
	BOOST_TEST(!geometry.draw_fits(tag, 3, k_max, 6));

	BOOST_TEST(geometry.draw_fits(tag, 0, k_max, k_max));
	BOOST_TEST(geometry.draw_fits(res::tag{ "memory://geom_range/never_built.desc" }, 3, 0, 0));

	// asked again, a range answers the same
	BOOST_TEST(geometry.draw_fits(tag, 3, 4, 6));
	BOOST_TEST(!geometry.draw_fits(tag, 3, 5, 6));
}

BOOST_AUTO_TEST_CASE(AGeometryWithAZeroVertexStride_HasNoVertexToDraw)
{
	rnd::geom_manager geometry(gpu(), storage.require<res::resource_system>());
	const res::tag tag = build(geometry, "no_layout.desc", "{}", 36, "[0,1,2]");

	BOOST_TEST(!geometry.draw_fits(tag, 3, 0, 0));
	BOOST_TEST(!geometry.draw_fits(tag, 1, 0, 0));
	BOOST_TEST(geometry.draw_fits(tag, 0, 0, 0));
}

BOOST_AUTO_TEST_CASE(RangesPastTheCapOfOneGeometry_AreStillChecked)
{
	rnd::geom_manager geometry(gpu(), storage.require<res::resource_system>());
	const res::tag tag = build(geometry, "many_ranges.desc", R"json({"position":"vec3"})json", 3 * 12, "[0,1,2]");

	const std::uint32_t asked = static_cast<std::uint32_t>(rnd::k_checked_ranges_cap) + 16u;
	for (std::uint32_t i = 0; i < asked; ++i) {
		BOOST_REQUIRE(!geometry.draw_fits(tag, 3, 1u + i, 0));
	}
	BOOST_TEST(geometry.draw_fits(tag, 3, 0, 0));
	BOOST_TEST(geometry.draw_fits(tag, 1, 2, 0));
	BOOST_TEST(!geometry.draw_fits(tag, 1, 3, 0));
	BOOST_TEST(!geometry.draw_fits(tag, 3, asked, 0));
	BOOST_TEST(!geometry.draw_fits(tag, 2, 0, 2));
}

BOOST_AUTO_TEST_SUITE_END()
