#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "rnd_null_driver.h"
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/render/rnd_capture_commands.h>
#include <snk/render/rnd_capture_service.h>
#include <snk/render/rnd_image_compare.h>
#include <snk/driver/rnd_texture_owner.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

// rnd::compare_rgba8 and the capture.compare command on the null backend (pixel (x, y) of the driver, bottom row
// first, is {x, y, 0, 255}). The references live in RAM under res://, decoded by the real picture adapter.

namespace {

namespace json = boost::json;
using namespace rnd::driver;

std::vector<std::byte> pixels(std::initializer_list<int> values)
{
	std::vector<std::byte> out;
	out.reserve(values.size());
	for (const int value : values) {
		out.push_back(static_cast<std::byte>(value));
	}
	return out;
}

rnd::rgba8_view view_of(int width, int height, const std::vector<std::byte>& rgba)
{
	return { width, height, rgba };
}

texture_header target_header(std::uint32_t width, std::uint32_t height)
{
	texture_header header;
	header.data.extent.width = width;
	header.data.extent.height = height;
	header.usage = texture_usage::color_target | texture_usage::sampled;
	return header;
}

struct compare_fixture : test_util::ram_res_env
{
	null::driver drv;
	texture_owner texture = make_texture(drv, target_header(4, 2));
	rnd::capture_service service{ drv };
	cmd::registration game = service.add_target("game", [this] { return texture.handle(); });
	test_util::ram_resolver* files = nullptr;
	cmd::command_registry commands;
	cmd::registration command;

	compare_fixture()
	{
		rs->unregistrate_resolver(res::tag::default_protocol());
		files = &rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
		command = rnd::register_capture_compare_command(commands, service, *rs);
	}

	// The capture of the game view as viewport.capture saves it.
	std::vector<std::byte> game_png()
	{
		const rnd::capture_result captured = service.capture("game");
		BOOST_REQUIRE(captured.ok());
		return rnd::capture_service::encode_png(captured.image);
	}

	void put(const std::string& tag, const std::vector<std::byte>& data)
	{
		files->files[res::tag{ tag }] = data;
	}

	void put_memory(const std::string& tag, const std::vector<std::byte>& data)
	{
		ram->files[res::tag{ tag }] = data;
	}

	cmd::result compare(const std::string& args)
	{
		return commands.invoke("capture.compare", json::parse(args), cmd::origin::test());
	}
};

}

BOOST_AUTO_TEST_SUITE(RndImageCompareTests)

BOOST_AUTO_TEST_CASE(IdenticalImages_HaveNoDifference)
{
	const auto a = pixels({ 1, 2, 3, 255, 200, 100, 50, 255 });
	const auto diff = rnd::compare_rgba8(view_of(2, 1, a), view_of(2, 1, a), 0.0);
	BOOST_REQUIRE(diff.has_value());
	BOOST_CHECK_EQUAL(diff->diff_pixels, 0);
	BOOST_CHECK_EQUAL(diff->total_pixels, 2);
	BOOST_CHECK_EQUAL(diff->diff_pct, 0.0);
}

BOOST_AUTO_TEST_CASE(OneChannel_JustUnderAndJustOverTheFuzz)
{
	// 4 % of 255 is 10.2: a step of 10 is within it, 11 is not.
	const auto base = pixels({ 100, 100, 100, 255 });
	const auto under = pixels({ 100, 110, 100, 255 });
	const auto over = pixels({ 100, 100, 89, 255 });
	BOOST_CHECK_EQUAL(rnd::compare_rgba8(view_of(1, 1, base), view_of(1, 1, under), 4.0)->diff_pixels, 0);
	BOOST_CHECK_EQUAL(rnd::compare_rgba8(view_of(1, 1, base), view_of(1, 1, over), 4.0)->diff_pixels, 1);
	BOOST_CHECK_EQUAL(rnd::compare_rgba8(view_of(1, 1, base), view_of(1, 1, under), 0.0)->diff_pixels, 1);

	const auto alpha_off = pixels({ 100, 100, 100, 200 });
	BOOST_CHECK_EQUAL(rnd::compare_rgba8(view_of(1, 1, base), view_of(1, 1, alpha_off), 4.0)->diff_pixels, 1);
}

BOOST_AUTO_TEST_CASE(DiffPct_IsTheShareOfDifferentPixels)
{
	const auto a = pixels({ 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 });
	const auto b = pixels({ 0, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 });
	const auto diff = rnd::compare_rgba8(view_of(2, 2, a), view_of(2, 2, b), 4.0);
	BOOST_REQUIRE(diff.has_value());
	BOOST_CHECK_EQUAL(diff->diff_pixels, 1);
	BOOST_CHECK_EQUAL(diff->total_pixels, 4);
	BOOST_CHECK_CLOSE(diff->diff_pct, 25.0, 1e-9);
}

BOOST_AUTO_TEST_CASE(DifferentSizesOrShortBuffers_AreNotCompared)
{
	const auto four = pixels({ 0, 0, 0, 255, 0, 0, 0, 255 });
	BOOST_CHECK(!rnd::compare_rgba8(view_of(2, 1, four), view_of(1, 2, four), 0.0).has_value());
	BOOST_CHECK(!rnd::compare_rgba8(view_of(2, 2, four), view_of(2, 2, four), 0.0).has_value());
	BOOST_CHECK_EQUAL(rnd::compare_rgba8(view_of(0, 0, {}), view_of(0, 0, {}), 0.0)->diff_pct, 0.0);
}

BOOST_AUTO_TEST_CASE(MissingAlpha_Is255)
{
	const auto rgb = pixels({ 10, 20, 30 });
	BOOST_CHECK(rnd::to_rgba8_top_down(rgb, 1, 1, 3, false) == pixels({ 10, 20, 30, 255 }));
	const auto grey = pixels({ 7 });
	BOOST_CHECK(rnd::to_rgba8_top_down(grey, 1, 1, 1, false) == pixels({ 7, 7, 7, 255 }));
	const auto grey_alpha = pixels({ 7, 9 });
	BOOST_CHECK(rnd::to_rgba8_top_down(grey_alpha, 1, 1, 2, false) == pixels({ 7, 7, 7, 9 }));
	BOOST_CHECK(rnd::to_rgba8_top_down(rgb, 1, 1, 5, false).empty());
	BOOST_CHECK(rnd::to_rgba8_top_down(rgb, 2, 1, 3, false).empty());
}

BOOST_AUTO_TEST_CASE(BottomUpRows_ComeOutTopFirst)
{
	const auto bottom_up = pixels({ 1, 1, 1, 2, 2, 2 });
	BOOST_CHECK(rnd::to_rgba8_top_down(bottom_up, 1, 2, 3, true) == pixels({ 2, 2, 2, 255, 1, 1, 1, 255 }));
	BOOST_CHECK(rnd::to_rgba8_top_down(bottom_up, 1, 2, 3, false) == pixels({ 1, 1, 1, 255, 2, 2, 2, 255 }));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(RndCaptureCompareCommandTests, compare_fixture)

BOOST_AUTO_TEST_CASE(Registered_AsAReadThatNeedsADisplay_WithStrictSchemas)
{
	BOOST_REQUIRE(command.active());
	const cmd::command_desc* desc = commands.find("capture.compare");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST((desc->category == cmd::agent_category::read));
	BOOST_CHECK(desc->needs_display);
	BOOST_CHECK(!desc->mutates);
	BOOST_CHECK(desc->risk == cmd::danger::none);
	BOOST_CHECK(!desc->args_schema.as_object().at("additionalProperties").as_bool());
	BOOST_CHECK(!desc->result_schema.as_object().at("additionalProperties").as_bool());
	BOOST_CHECK(desc->args_schema.as_object().at("required").as_array() == json::array{ "reference" });

	BOOST_CHECK_EQUAL(compare("{}").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://r.png","scale":2})json").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://r.png","fuzz":101})json").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://r.png","max_diff_pct":-1})json").code, cmd::codes::invalid_args);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://r.png","fuzz":"4%"})json").code, cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(Headless_IsUnavailable)
{
	put("res://refs/game.png", game_png());
	commands.set_headless(true);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/game.png"})json").code, cmd::codes::unavailable_headless);
}

BOOST_AUTO_TEST_CASE(ReferenceSavedFromTheCapture_Matches)
{
	put("res://refs/game.png", game_png());

	const auto result = compare(R"json({"reference":"res://refs/game.png","fuzz":0,"max_diff_pct":0})json");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const json::object& value = result.value.as_object();
	BOOST_CHECK(value.at("match").as_bool());
	BOOST_CHECK_EQUAL(value.at("diff_pixels").as_int64(), 0);
	BOOST_CHECK_EQUAL(value.at("total_pixels").as_int64(), 8);
	BOOST_CHECK_EQUAL(value.at("width").as_int64(), 4);
	BOOST_CHECK_EQUAL(value.at("height").as_int64(), 2);
	BOOST_CHECK_EQUAL(std::string(value.at("viewport").as_string()), "game");
	BOOST_CHECK_EQUAL(std::string(value.at("reference").as_string()), "res://refs/game.png");
}

BOOST_AUTO_TEST_CASE(UpsideDownReference_DiffersEverywhere)
{
	// Rows of the null picture differ in green by one: a comparison that flipped one side would match this one.
	rnd::captured_image image = service.capture("game").image;
	const std::size_t stride = static_cast<std::size_t>(image.size.x) * 4;
	std::vector<std::byte> flipped(image.rgba.size());
	std::memcpy(flipped.data(), image.rgba.data() + stride, stride);
	std::memcpy(flipped.data() + stride, image.rgba.data(), stride);
	image.rgba = flipped;
	put("res://refs/flipped.png", rnd::capture_service::encode_png(image));

	const auto result = compare(R"json({"reference":"res://refs/flipped.png","fuzz":0})json");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const json::object& value = result.value.as_object();
	BOOST_CHECK(!value.at("match").as_bool());
	BOOST_CHECK_EQUAL(value.at("diff_pixels").as_int64(), 8);
	BOOST_CHECK_CLOSE(value.at("diff_pct").as_double(), 100.0, 1e-9);

	// One green step is under the default fuzz (4 % of 255).
	BOOST_CHECK(compare(R"json({"reference":"res://refs/flipped.png"})json").value.as_object().at("match").as_bool());
}

BOOST_AUTO_TEST_CASE(SizeMismatch_NamesBothSizes)
{
	rnd::captured_image image;
	image.size = { 3, 2 };
	image.rgba.assign(3 * 2 * 4, std::byte{ 255 });
	put("res://refs/small.png", rnd::capture_service::encode_png(image));

	const auto result = compare(R"json({"reference":"res://refs/small.png"})json");

	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, rnd::capture_size_mismatch);
	BOOST_CHECK_EQUAL(result.code, "size_mismatch");
	BOOST_CHECK(result.error.find("4x2") != std::string::npos);
	BOOST_CHECK(result.error.find("3x2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(BadReferenceTags_AreRefusedBeforeTheResourceSystem)
{
	// Each of these holds a picture that would match: a refusal means the tag was never loaded.
	const auto png = game_png();
	for (const char* tag : { "res://a/../refs/game.png", "res://./refs/game.png", "res:///refs/game.png", "res://C:/refs/game.png" }) {
		put(tag, png);
	}
	put_memory("memory://refs/game.png", png);

	for (const char* reference : { "res://a/../refs/game.png", "res://./refs/game.png", "res:///refs/game.png", "res://C:/refs/game.png",
			"res://a\\..\\refs\\game.png", "memory://refs/game.png", "project://refs/game.png", "/refs/game.png", "C:/refs/game.png",
			"refs/game.png", "res://", "res://refs/game.desc" }) {
		BOOST_TEST_CONTEXT(reference) {
			const json::value args = json::object{ { "reference", reference } };
			const auto result = commands.invoke("capture.compare", args, cmd::origin::test());
			BOOST_CHECK(!result.ok);
			BOOST_CHECK_EQUAL(result.code, cmd::codes::invalid_args);
		}
	}
}

BOOST_AUTO_TEST_CASE(MissingOrBrokenReference_Fails)
{
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/none.png"})json").code, cmd::codes::failed);
	put("res://refs/broken.png", test_util::to_bytes("not a picture"));
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/broken.png"})json").code, cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(UnknownViewport_IsNotFound)
{
	put("res://refs/game.png", game_png());
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/game.png","viewport":"scene"})json").code, cmd::codes::not_found);
	game.reset();
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/game.png"})json").code, cmd::codes::not_found);
}

BOOST_AUTO_TEST_SUITE_END()
