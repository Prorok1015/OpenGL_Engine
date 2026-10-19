#include <boost/test/unit_test.hpp>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <type_traits>
#include <vector>

// The draw packet is plain data (R6a, #124): it is copied, sorted and kept in frame memory without owning
// anything, and its sort key orders pass, then pipeline, then material, then depth.

namespace {

static_assert(std::is_trivially_copyable_v<rnd::draw_call_t>);
static_assert(std::is_trivially_destructible_v<rnd::draw_call_t>);
static_assert(std::is_standard_layout_v<rnd::draw_call_t>);
static_assert(std::is_trivially_copyable_v<rnd::geometry_id>);
static_assert(std::is_trivially_copyable_v<rnd::skin_id>);
static_assert(std::is_trivially_copyable_v<rnd::material_instance_id>);
static_assert(std::is_trivially_copyable_v<rnd::variant_id>);

constexpr rnd::draw_pass opaque = rnd::draw_pass::opaque;
constexpr rnd::draw_pass transparent = rnd::draw_pass::transparent;

rnd::draw_call_t draw_with_key(std::uint64_t key, std::uint32_t marker)
{
	rnd::draw_call_t draw;
	draw.sort_key = key;
	draw.bone_offset = marker;
	return draw;
}

}

BOOST_AUTO_TEST_SUITE(RndDrawCallTests)

BOOST_AUTO_TEST_CASE(DefaultDrawNamesNothing)
{
	const rnd::draw_call_t draw;
	BOOST_TEST(!draw.geometry.is_valid());
	BOOST_TEST(!draw.material.is_valid());
	BOOST_TEST(!draw.skin.is_valid());
	BOOST_TEST(!draw.variant.is_valid());
	BOOST_TEST(draw.bone_count == 0u);
}

BOOST_AUTO_TEST_CASE(DrawCallSurvivesAMemcpy)
{
	rnd::draw_call_t draw;
	draw.sort_key = 0x1234567890abcdefull;
	draw.geometry.index = 3;
	draw.material.index = 4;
	draw.skin.index = 5;
	draw.variant.value = 6;
	draw.indices_count = 7;
	draw.vx_begin = 8;
	draw.ind_begin = 9;
	draw.transform[3][0] = 10.0f;
	draw.bone_offset = 11;
	draw.bone_count = 12;

	rnd::draw_call_t copy;
	std::memcpy(&copy, &draw, sizeof(copy));
	BOOST_TEST(copy.sort_key == draw.sort_key);
	BOOST_CHECK(copy.geometry == draw.geometry);
	BOOST_CHECK(copy.material == draw.material);
	BOOST_CHECK(copy.skin == draw.skin);
	BOOST_CHECK(copy.variant == draw.variant);
	BOOST_CHECK(copy.transform == draw.transform);
	BOOST_TEST(copy.bone_offset == 11u);
	BOOST_TEST(copy.bone_count == 12u);
}

BOOST_AUTO_TEST_CASE(DepthQuantizationGrowsWithDistance)
{
	BOOST_TEST(rnd::quantize_view_depth(0.0f) == 0u);
	BOOST_TEST(rnd::quantize_view_depth(-5.0f) == 0u);	// behind the camera
	BOOST_TEST(rnd::quantize_view_depth(std::numeric_limits<float>::quiet_NaN()) == 0u);
	BOOST_TEST(rnd::quantize_view_depth(1.0f) < rnd::quantize_view_depth(2.0f));
	BOOST_TEST(rnd::quantize_view_depth(2.0f) < rnd::quantize_view_depth(100.0f));
	BOOST_TEST(rnd::quantize_view_depth(100.0f) < rnd::quantize_view_depth(10000.0f));
	BOOST_TEST(rnd::quantize_view_depth(std::numeric_limits<float>::infinity()) < (1u << rnd::sort_key_depth_bits));
}

BOOST_AUTO_TEST_CASE(OpaqueKeyOrdersPassThenPipelineThenMaterialThenDepth)
{
	// The pass beats everything: the worst opaque key still sorts before the best transparent one.
	BOOST_TEST(rnd::make_sort_key(opaque, (1u << rnd::sort_key_pipeline_bits) - 1, (1u << rnd::sort_key_material_bits) - 1, 1e9f) <
		rnd::make_sort_key(transparent, 0, 0, 1e9f));

	// Pipeline beats material and depth.
	BOOST_TEST(rnd::make_sort_key(opaque, 1, 900, 500.0f) < rnd::make_sort_key(opaque, 2, 0, 0.5f));
	// Material beats depth.
	BOOST_TEST(rnd::make_sort_key(opaque, 4, 1, 500.0f) < rnd::make_sort_key(opaque, 4, 2, 0.5f));
	// Within a material the nearest draw comes first.
	BOOST_TEST(rnd::make_sort_key(opaque, 4, 2, 0.5f) < rnd::make_sort_key(opaque, 4, 2, 500.0f));
	// Equal inputs, equal key.
	BOOST_TEST(rnd::make_sort_key(opaque, 4, 2, 7.0f) == rnd::make_sort_key(opaque, 4, 2, 7.0f));
}

BOOST_AUTO_TEST_CASE(TransparentKeyIsBackToFrontThenPipelineThenMaterial)
{
	// Depth leads: a far draw is drawn before a near one whatever its pipeline and material.
	BOOST_TEST(rnd::make_sort_key(transparent, 900, 900, 50.0f) < rnd::make_sort_key(transparent, 1, 1, 5.0f));
	BOOST_TEST(rnd::make_sort_key(transparent, 1, 1, 50.0f) < rnd::make_sort_key(transparent, 1, 1, 5.0f));
	// At the same depth the pipeline, then the material, group draws.
	BOOST_TEST(rnd::make_sort_key(transparent, 1, 900, 5.0f) < rnd::make_sort_key(transparent, 2, 0, 5.0f));
	BOOST_TEST(rnd::make_sort_key(transparent, 2, 1, 5.0f) < rnd::make_sort_key(transparent, 2, 2, 5.0f));
}

BOOST_AUTO_TEST_CASE(SortDrawsOrdersByKeyAndKeepsExtractionOrderOfEqualKeys)
{
	std::vector<rnd::draw_call_t> draws;
	draws.push_back(draw_with_key(rnd::make_sort_key(opaque, 2, 1, 3.0f), 0));
	draws.push_back(draw_with_key(rnd::make_sort_key(opaque, 1, 5, 9.0f), 1));
	draws.push_back(draw_with_key(rnd::make_sort_key(opaque, 1, 5, 9.0f), 2));	// ties with the one before
	draws.push_back(draw_with_key(rnd::make_sort_key(opaque, 1, 2, 9.0f), 3));
	draws.push_back(draw_with_key(rnd::make_sort_key(opaque, 1, 5, 9.0f), 4));	// ties again

	rnd::sort_draws(draws);

	BOOST_REQUIRE(std::ranges::is_sorted(draws, {}, &rnd::draw_call_t::sort_key));
	const std::vector<std::uint32_t> expected{ 3, 1, 2, 4, 0 };
	for (std::size_t i = 0; i < expected.size(); ++i) {
		BOOST_TEST_CONTEXT("draw " << i) {
			BOOST_TEST(draws[i].bone_offset == expected[i]);
		}
	}
}

BOOST_AUTO_TEST_CASE(SortedTransparentDrawsComeFarthestFirst)
{
	std::vector<rnd::draw_call_t> draws;
	const float depths[] = { 4.0f, 40.0f, 0.5f, 12.0f, 400.0f };
	for (std::uint32_t i = 0; i < std::size(depths); ++i) {
		// Different pipelines and materials must not disturb the depth order.
		draws.push_back(draw_with_key(rnd::make_sort_key(transparent, i % 2, i * 3 % 5, depths[i]), i));
	}

	rnd::sort_draws(draws);

	const std::vector<std::uint32_t> expected{ 4, 1, 3, 0, 2 };
	for (std::size_t i = 0; i < expected.size(); ++i) {
		BOOST_TEST_CONTEXT("draw " << i) {
			BOOST_TEST(draws[i].bone_offset == expected[i]);
		}
	}
}

BOOST_AUTO_TEST_CASE(PacketResolvesIdsAgainstItsTables)
{
	rnd::render_packet_t packet;
	packet.geometries.push_back(res::tag::make("geometry/a"));
	packet.geometries.push_back(res::tag::make("geometry/b"));
	packet.skins.push_back(res::tag::make("skins/a"));

	rnd::draw_call_t draw;
	draw.geometry.index = 1;
	BOOST_TEST(packet.geometry_of(draw) == res::tag::make("geometry/b"));
	BOOST_TEST(!packet.skin_of(draw).is_valid());	// a static draw has no skin

	draw.skin.index = 0;
	BOOST_TEST(packet.skin_of(draw) == res::tag::make("skins/a"));
}

BOOST_AUTO_TEST_SUITE_END()
