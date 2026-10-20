#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/driver/rnd_texture_owner.h>
#include <boost/json.hpp>

// Usage flags of a texture: the bit mask itself, its JSON forms (a string as the shipped .desc files
// have it, or an array) and the render_targets::ensure helper the passes share.

namespace {

using namespace rnd::driver;
namespace json = boost::json;

struct usage_fixture : test_util::null_engine_fixture
{
	texture_header read_header(const char* text)
	{
		json::object resource;
		resource["header"] = json::parse(text);
		rnd::texture_desc desc;
		desc.deserialize(storage.require<desc::desc_system>(), resource);
		return desc.header;
	}

	json::value write_header(const texture_header& header)
	{
		rnd::texture_desc desc;
		desc.header = header;
		json::object resource;
		desc.serialize(resource);
		return resource.at("header");
	}
};

}

BOOST_FIXTURE_TEST_SUITE(RndTextureUsageTests, usage_fixture)

BOOST_AUTO_TEST_CASE(UsageIsABitMask)
{
	constexpr auto usage = texture_usage::color_target | texture_usage::sampled;
	BOOST_TEST(has_usage(usage, texture_usage::sampled));
	BOOST_TEST(has_usage(usage, texture_usage::color_target));
	BOOST_TEST(has_usage(usage, usage));
	BOOST_TEST(!has_usage(usage, texture_usage::depth_target));
	BOOST_TEST(!has_usage(usage, texture_usage::sampled | texture_usage::depth_target));
	BOOST_TEST(!has_usage(usage, texture_usage::none));
	BOOST_TEST(has_any_usage(usage, texture_usage::depth_target | texture_usage::sampled));
	BOOST_TEST(!has_any_usage(usage, texture_usage::depth_target | texture_usage::stencil_target));
}

BOOST_AUTO_TEST_CASE(TextureQueriesEveryFlagIndependently)
{
	texture_header header;
	header.usage = texture_usage::color_target | texture_usage::sampled;
	rnd::driver::null::driver null_driver;
	auto texture = make_texture(null_driver, header);
	BOOST_REQUIRE(texture);
	BOOST_TEST(texture->has_usage(texture_usage::sampled));
	BOOST_TEST(texture->has_usage(texture_usage::color_target));
	BOOST_TEST(!texture->has_usage(texture_usage::depth_target));
}

BOOST_AUTO_TEST_CASE(ASingleUsageStringIsStillRead)
{
	BOOST_TEST((read_header(R"({"usage": "sampled"})").usage == texture_usage::sampled));
	BOOST_TEST((read_header(R"({"usage": "depth_target"})").usage == texture_usage::depth_target));
	BOOST_TEST((read_header(R"({"usage": "bogus"})").usage == texture_usage::none));
	BOOST_TEST((read_header(R"({})").usage == texture_usage::sampled));
}

BOOST_AUTO_TEST_CASE(AnArrayOfUsagesIsRead)
{
	const auto header = read_header(R"({"usage": ["color_target", "sampled", "nonsense"]})");
	BOOST_TEST((header.usage == (texture_usage::color_target | texture_usage::sampled)));
}

BOOST_AUTO_TEST_CASE(ASingleUsageIsWrittenAsAStringLikeBefore)
{
	texture_header header;
	for (const auto usage : { texture_usage::sampled, texture_usage::storage, texture_usage::color_target,
		texture_usage::depth_target, texture_usage::stencil_target }) {
		header.usage = usage;
		const auto written = write_header(header);
		BOOST_REQUIRE(written.as_object().at("usage").is_string());
		BOOST_TEST((read_header(json::serialize(written).c_str()).usage == usage));
	}
	header.usage = texture_usage::none;
	BOOST_TEST(write_header(header).as_object().at("usage").as_string() == "unknown");
}

BOOST_AUTO_TEST_CASE(SeveralUsagesAreWrittenAsAnArrayAndRoundTrip)
{
	texture_header header;
	header.usage = texture_usage::color_target | texture_usage::sampled;
	const auto written = write_header(header);
	BOOST_REQUIRE(written.as_object().at("usage").is_array());
	BOOST_TEST(written.as_object().at("usage").as_array().size() == 2u);
	BOOST_TEST((read_header(json::serialize(written).c_str()).usage == header.usage));
}

BOOST_AUTO_TEST_CASE(EnsureKeepsATargetOfTheSameSize)
{
	auto& textures = storage.require<rnd::render_system>().get_texture_manager();
	const auto tag = res::tag(res::tag::memory, "__rt_keep");
	const auto usage = texture_usage::color_target | texture_usage::sampled;
	auto* first = rnd::render_targets::ensure(textures, tag, { 64, 32 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(first);
	BOOST_TEST(first->width() == 64u);
	BOOST_TEST(first->height() == 32u);
	BOOST_TEST((first->get_usage() == usage));
	const auto generation = textures.generation();
	BOOST_TEST(rnd::render_targets::ensure(textures, tag, { 64, 32 }, texture_header::TYPE::RGBA8, usage) == first);
	BOOST_TEST(textures.generation() == generation);
}

BOOST_AUTO_TEST_CASE(EnsureRecreatesATargetWhenTheSizeChanges)
{
	auto& textures = storage.require<rnd::render_system>().get_texture_manager();
	const auto tag = res::tag(res::tag::memory, "__rt_resize");
	rnd::render_targets::ensure(textures, tag, { 64, 32 }, texture_header::TYPE::RGBA8, texture_usage::color_target);
	const auto generation = textures.generation();
	auto* resized = rnd::render_targets::ensure(textures, tag, { 128, 96 }, texture_header::TYPE::RGBA8, texture_usage::color_target);
	BOOST_REQUIRE(resized);
	BOOST_TEST(resized->width() == 128u);
	BOOST_TEST(resized->height() == 96u);
	BOOST_TEST(textures.find(tag) == resized);
	BOOST_TEST(textures.generation() > generation);
}

BOOST_AUTO_TEST_CASE(RegistryKeepsTheSlotAndBumpsTheGenerationOnResize)
{
	auto& system = storage.require<rnd::render_system>();
	auto& textures = system.get_texture_manager();
	rnd::render_targets::registry registry;
	const auto tag = res::tag(res::tag::memory, "__rt_registry");
	const auto usage = texture_usage::color_target | texture_usage::sampled;

	const auto first = registry.ensure(textures, tag, { 64, 32 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(first);
	BOOST_CHECK(registry.ensure(textures, tag, { 64, 32 }, texture_header::TYPE::RGBA8, usage) == first);
	const texture_handle old_handle = registry.resolve(first);
	BOOST_REQUIRE(system.get_driver()->is_alive(old_handle));

	const auto resized = registry.ensure(textures, tag, { 128, 96 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(resized);
	BOOST_TEST(resized.slot == first.slot);
	BOOST_TEST(resized.gen > first.gen);
	// The id kept across the resize is stale; it does not resolve to the new texture, nor to the freed one.
	BOOST_CHECK(!registry.is_current(first));
	BOOST_CHECK(!registry.resolve(first));
	BOOST_CHECK(!system.get_driver()->is_alive(old_handle));
	BOOST_CHECK(registry.is_current(resized));
	const texture_handle current = registry.resolve(resized);
	BOOST_REQUIRE(system.get_driver()->is_alive(current));
	BOOST_TEST(system.get_driver()->info(current).data.extent.width == 128u);
	BOOST_CHECK(registry.find(tag) == resized);
	BOOST_CHECK(registry.current(tag) == current);
}

BOOST_AUTO_TEST_CASE(RegistryIdsAreNullForAnUnknownTargetAndPerTag)
{
	auto& textures = storage.require<rnd::render_system>().get_texture_manager();
	rnd::render_targets::registry registry;
	const auto usage = texture_usage::color_target | texture_usage::sampled;
	const auto a_tag = res::tag(res::tag::memory, "__rt_registry_a");
	const auto b_tag = res::tag(res::tag::memory, "__rt_registry_b");

	BOOST_CHECK(!registry.find(a_tag));
	BOOST_CHECK(!registry.current(a_tag));
	BOOST_CHECK(!registry.resolve({}));
	BOOST_CHECK(!registry.is_current({ 7, 1 }));
	const auto a = registry.ensure(textures, a_tag, { 8, 8 }, texture_header::TYPE::RGBA8, usage);
	const auto b = registry.ensure(textures, b_tag, { 8, 8 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(b);
	BOOST_TEST(a.slot != b.slot);
	BOOST_CHECK(registry.resolve(a) != registry.resolve(b));
}

BOOST_AUTO_TEST_CASE(EnsureCreatesTargetsThatPassValidation)
{
	auto& textures = storage.require<rnd::render_system>().get_texture_manager();
	rnd::render_targets::ensure(textures, res::tag(res::tag::memory, "__rt_depth"), { 16, 16 }, texture_header::TYPE::D32, texture_usage::depth_target);
	rnd::render_targets::ensure(textures, res::tag(res::tag::memory, "__rt_color"), { 16, 16 }, texture_header::TYPE::RGBA16F,
		texture_usage::color_target | texture_usage::sampled);
	BOOST_TEST(validating_driver()->state().total() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
