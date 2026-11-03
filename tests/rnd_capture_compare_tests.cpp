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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// rnd::compare_rgb_of_rgba8 and the capture.compare command on the null backend (pixel (x, y) of the driver, bottom row
// first, is {x, y, 0, 255}). The references live in RAM under res://, read as bytes and decoded by the command.

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

std::uint32_t crc32_of(std::span<const std::byte> bytes)
{
	std::uint32_t crc = 0xFFFFFFFFu;
	for (const std::byte value : bytes) {
		crc ^= std::to_integer<std::uint32_t>(value);
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

void put_be32(std::vector<std::byte>& out, std::uint32_t value)
{
	for (const int shift : { 24, 16, 8, 0 }) {
		out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
	}
}

void put_chunk(std::vector<std::byte>& out, const char (&type)[5], const std::vector<std::byte>& data)
{
	put_be32(out, static_cast<std::uint32_t>(data.size()));
	const std::size_t type_at = out.size();
	for (std::size_t i = 0; i < 4; ++i) {
		out.push_back(static_cast<std::byte>(type[i]));
	}
	out.insert(out.end(), data.begin(), data.end());
	put_be32(out, crc32_of(std::span<const std::byte>(out).subspan(type_at)));
}

std::vector<std::byte> ihdr_of(std::uint32_t width, std::uint32_t height)
{
	std::vector<std::byte> data;
	put_be32(data, width);
	put_be32(data, height);
	// 8 bits, RGBA, deflate, no filter method, no interlace.
	for (const int value : { 8, 6, 0, 0, 0 }) {
		data.push_back(static_cast<std::byte>(value));
	}
	return data;
}

// A PNG signature and an IHDR with a valid CRC, no pixels: all a header read looks at.
std::vector<std::byte> png_header_only(std::uint32_t width, std::uint32_t height)
{
	std::vector<std::byte> out = pixels({ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
	put_chunk(out, "IHDR", ihdr_of(width, height));
	return out;
}

// An RGBA PNG of `rgba` (top row first), its pixels in one stored (uncompressed) deflate block.
std::vector<std::byte> rgba_png(int width, int height, const std::vector<std::byte>& rgba)
{
	const std::size_t stride = static_cast<std::size_t>(width) * 4;
	std::vector<std::byte> raw;
	raw.reserve((stride + 1) * static_cast<std::size_t>(height));
	for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
		raw.push_back(std::byte{ 0 });
		raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y * stride), rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * stride));
	}
	std::uint32_t a = 1;
	std::uint32_t b = 0;
	for (const std::byte value : raw) {
		a = (a + std::to_integer<std::uint32_t>(value)) % 65521u;
		b = (b + a) % 65521u;
	}
	const std::uint32_t length = static_cast<std::uint32_t>(raw.size());
	std::vector<std::byte> zlib = pixels({ 0x78, 0x01, 0x01 });
	for (const std::uint32_t half : { length, ~length }) {
		zlib.push_back(static_cast<std::byte>(half & 0xFFu));
		zlib.push_back(static_cast<std::byte>((half >> 8) & 0xFFu));
	}
	zlib.insert(zlib.end(), raw.begin(), raw.end());
	put_be32(zlib, (b << 16) | a);

	std::vector<std::byte> out = pixels({ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
	put_chunk(out, "IHDR", ihdr_of(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
	put_chunk(out, "IDAT", zlib);
	put_chunk(out, "IEND", {});
	return out;
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
	const auto diff = rnd::compare_rgb_of_rgba8(view_of(2, 1, a), view_of(2, 1, a), 0.0);
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
	BOOST_CHECK_EQUAL(rnd::compare_rgb_of_rgba8(view_of(1, 1, base), view_of(1, 1, under), 4.0)->diff_pixels, 0);
	BOOST_CHECK_EQUAL(rnd::compare_rgb_of_rgba8(view_of(1, 1, base), view_of(1, 1, over), 4.0)->diff_pixels, 1);
	BOOST_CHECK_EQUAL(rnd::compare_rgb_of_rgba8(view_of(1, 1, base), view_of(1, 1, under), 0.0)->diff_pixels, 1);

	// Alpha is not compared.
	const auto alpha_off = pixels({ 100, 100, 100, 0 });
	BOOST_CHECK_EQUAL(rnd::compare_rgb_of_rgba8(view_of(1, 1, base), view_of(1, 1, alpha_off), 0.0)->diff_pixels, 0);
}

BOOST_AUTO_TEST_CASE(Crc32Helper_MatchesTheStandardCheckValue)
{
	const std::string check = "123456789";
	BOOST_CHECK_EQUAL(crc32_of(std::as_bytes(std::span(check))), 0xCBF43926u);
}

BOOST_AUTO_TEST_CASE(DiffPct_IsTheShareOfDifferentPixels)
{
	const auto a = pixels({ 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 });
	const auto b = pixels({ 0, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 });
	const auto diff = rnd::compare_rgb_of_rgba8(view_of(2, 2, a), view_of(2, 2, b), 4.0);
	BOOST_REQUIRE(diff.has_value());
	BOOST_CHECK_EQUAL(diff->diff_pixels, 1);
	BOOST_CHECK_EQUAL(diff->total_pixels, 4);
	BOOST_CHECK_CLOSE(diff->diff_pct, 25.0, 1e-9);
}

BOOST_AUTO_TEST_CASE(DifferentSizesOrShortBuffers_AreNotCompared)
{
	const auto four = pixels({ 0, 0, 0, 255, 0, 0, 0, 255 });
	BOOST_CHECK(!rnd::compare_rgb_of_rgba8(view_of(2, 1, four), view_of(1, 2, four), 0.0).has_value());
	BOOST_CHECK(!rnd::compare_rgb_of_rgba8(view_of(2, 2, four), view_of(2, 2, four), 0.0).has_value());
	BOOST_CHECK_EQUAL(rnd::compare_rgb_of_rgba8(view_of(0, 0, {}), view_of(0, 0, {}), 0.0)->diff_pct, 0.0);
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

	put("res://refs/game.pct", png);
	for (const char* reference : { "res://a/../refs/game.png", "res://./refs/game.png", "res:///refs/game.png", "res://C:/refs/game.png",
			"res://a\\..\\refs\\game.png", "memory://refs/game.png", "project://refs/game.png", "/refs/game.png", "C:/refs/game.png",
			"refs/game.png", "res://", "res://refs/game.desc", "res://refs/game.pct" }) {
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
	const auto missing = compare(R"json({"reference":"res://refs/none.png"})json");
	BOOST_CHECK_EQUAL(missing.code, cmd::codes::failed);
	// The resolver's error (a path on disk) is logged, not returned.
	BOOST_CHECK_EQUAL(missing.error, "the reference 'res://refs/none.png' cannot be loaded");

	// A PNG signature with no readable header.
	std::vector<std::byte> broken = pixels({ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
	const auto junk = test_util::to_bytes("not a picture header");
	broken.insert(broken.end(), junk.begin(), junk.end());
	put("res://refs/broken.png", broken);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/broken.png"})json").code, cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(NotAPngOrJpeg_IsRefusedByItsBytes)
{
	put("res://refs/text.png", test_util::to_bytes("not a picture"));
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/text.png"})json").code, cmd::codes::invalid_args);

	// stb would decode a GIF behind a .png name.
	put("res://refs/gif.png", pixels({ 'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, ';' }));
	const auto gif = compare(R"json({"reference":"res://refs/gif.png"})json");
	BOOST_CHECK_EQUAL(gif.code, cmd::codes::invalid_args);
	BOOST_CHECK(gif.error.find("PNG or JPEG") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HugeHeader_IsRefusedBeforeDecoding)
{
	// 3.6 G pixels: stb's own header read gives up on it, the command still names it as over the limits.
	put("res://refs/huge.png", png_header_only(60000, 60000));
	const auto huge = compare(R"json({"reference":"res://refs/huge.png"})json");
	BOOST_CHECK_EQUAL(huge.code, cmd::codes::invalid_args);
	BOOST_CHECK(huge.error.find("60000x60000") != std::string::npos);

	// Few pixels, one side too long.
	put("res://refs/long.png", png_header_only(20000, 1));
	const auto long_side = compare(R"json({"reference":"res://refs/long.png"})json");
	BOOST_CHECK_EQUAL(long_side.code, cmd::codes::invalid_args);
	BOOST_CHECK(long_side.error.find("20000x1") != std::string::npos);
	BOOST_CHECK(long_side.error.find("16384") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ReferenceOverTheSourceLimit_IsRefusedBeforeDecoding)
{
	put("res://refs/game.png", game_png());
	const auto refused = compare(R"json({"reference":"res://refs/game.png","max_source_pixels":7})json");
	BOOST_CHECK_EQUAL(refused.code, cmd::codes::invalid_args);
	BOOST_CHECK(refused.error.find("4x2") != std::string::npos);
	BOOST_CHECK(compare(R"json({"reference":"res://refs/game.png","max_source_pixels":8})json").ok);
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/game.png","max_source_pixels":0})json").code, cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(TooManyBytes_AreRefusedBeforeTheHeaderIsRead)
{
	std::vector<std::byte> big(rnd::capture_compare_max_reference_bytes + 1, std::byte{ 0 });
	const std::vector<std::byte> header = png_header_only(4, 2);
	std::copy(header.begin(), header.end(), big.begin());
	files->files[res::tag{ "res://refs/big.png" }] = std::move(big);

	const auto result = compare(R"json({"reference":"res://refs/big.png"})json");
	BOOST_CHECK_EQUAL(result.code, cmd::codes::invalid_args);
	BOOST_CHECK(result.error.find(std::to_string(rnd::capture_compare_max_reference_bytes)) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TransparentReference_ComparesByRgb)
{
	rnd::captured_image image = service.capture("game").image;
	for (std::size_t alpha = 3; alpha < image.rgba.size(); alpha += 8) {
		image.rgba[alpha] = std::byte{ 0 };
	}
	put("res://refs/transparent.png", rgba_png(image.size.x, image.size.y, image.rgba));

	const auto result = compare(R"json({"reference":"res://refs/transparent.png","fuzz":0,"max_diff_pct":0})json");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK(result.value.as_object().at("match").as_bool());
	BOOST_CHECK_EQUAL(result.value.as_object().at("diff_pixels").as_int64(), 0);
}

BOOST_AUTO_TEST_CASE(UnknownViewport_IsNotFound)
{
	put("res://refs/game.png", game_png());
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/game.png","viewport":"scene"})json").code, cmd::codes::not_found);
	game.reset();
	BOOST_CHECK_EQUAL(compare(R"json({"reference":"res://refs/game.png"})json").code, cmd::codes::not_found);
}

BOOST_AUTO_TEST_SUITE_END()
