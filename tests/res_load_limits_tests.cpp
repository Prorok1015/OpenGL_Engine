#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "png_bomb_builder.hpp"
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_json_body.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #928: a small file must not be able to ask for gigabytes: a picture is refused by its header before anything is
// decoded and gets a memory budget while it is, a desc is refused by its size before anything is parsed.
namespace {

struct load_limit_fixture : test_util::null_engine_fixture
{
	load_limit_fixture() : null_engine_fixture(false) {}

	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	template<class T>
	res::res_handle<T> load(std::string_view name, std::vector<std::byte> bytes)
	{
		std::string path = "memory://limits/";
		path.append(name);
		const res::tag tag{ path };
		BOOST_REQUIRE(resources().store(tag, bytes));
		auto handle = resources().require_sync<T>(tag);
		test_util::pump(tasks);
		return handle;
	}

	// The signature and the IHDR chunk of a PNG and nothing else: all a decoder needs to learn the size it would allocate.
	static std::vector<std::byte> png_header(std::uint32_t width, std::uint32_t height)
	{
		std::vector<unsigned char> bytes{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
		for (const std::uint32_t side : { width, height }) {
			for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<unsigned char>(side >> shift));
		}
		// 8 bits a channel, colour type 6 (RGBA), deflate, no filter, no interlace; a CRC nobody checks here.
		for (const unsigned char byte : { 8, 6, 0, 0, 0, 0, 0, 0, 0 }) bytes.push_back(byte);
		std::vector<std::byte> out(bytes.size());
		for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<std::byte>(bytes[i]);
		return out;
	}
};

std::vector<std::byte> as_bytes(std::string_view text)
{
	std::vector<std::byte> out(text.size());
	std::memcpy(out.data(), text.data(), text.size());
	return out;
}

using test_util::png_bomb::zlib_stream;

std::vector<std::byte> png_1x1(const std::vector<unsigned char>& idat)
{
	return test_util::png_bomb::png_rgba(1, 1, idat);
}

bool says(const std::exception& error, std::string_view text)
{
	return std::string_view{ error.what() }.find(text) != std::string_view::npos;
}

}

BOOST_FIXTURE_TEST_SUITE(ResLoadLimitsTests, load_limit_fixture)

BOOST_AUTO_TEST_CASE(APictureWhoseHeaderAsksForTooMuch_IsRefusedBeforeItIsDecoded)
{
	const std::uint32_t side = static_cast<std::uint32_t>(res::pct_adapter::max_side);
	// A side too long; then sides that fit with more pixel bytes than the budget (16384 x 16384 x 4 is 1 GiB).
	const std::pair<std::uint32_t, std::uint32_t> sizes[] = { { side + 1, 1 }, { 1, side + 1 }, { side, side } };
	std::size_t index = 0;
	for (const auto& [width, height] : sizes) {
		BOOST_TEST_CONTEXT(width << "x" << height) {
			const auto picture = load<res::picture_resource>("huge" + std::to_string(index++) + ".png", png_header(width, height));
			BOOST_TEST(picture.has_error());
			BOOST_TEST(picture.error_text().find("too large to decode") != std::string::npos);
		}
	}
}

BOOST_AUTO_TEST_CASE(APictureWithinTheBudget_GoesOnToTheDecoder)
{
	// The header fits, the pixels are missing: it is the decoder that refuses it, not the budget.
	const auto picture = load<res::picture_resource>("cut.png", png_header(64, 64));

	BOOST_TEST(picture.has_error());
	BOOST_TEST(picture.error_text().find("too large to decode") == std::string::npos);
	BOOST_TEST(picture.error_text().find("not a decodable image") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AOnePixelPng_DecodesWithinItsBudget)
{
	// Filter byte 0, then the pixel: proves the hand-made stream below is one stb reads.
	const std::vector<std::byte> bytes = png_1x1(zlib_stream({ 0, 10, 20, 30, 40 }, 0));
	const res::pct_adapter adapter;

	const auto picture = adapter(res::tag{ "memory://limits/pixel.png" }, bytes);

	BOOST_REQUIRE(picture);
	BOOST_TEST(picture->size().x == 1);
	BOOST_TEST(picture->size().y == 1);
	BOOST_REQUIRE(picture->channels() == 4);
	BOOST_TEST(picture->data()[0] == 10);
	BOOST_TEST(picture->data()[3] == 40);
	BOOST_TEST(res::pct_adapter::last_decode_peak_bytes() > 0u);
	BOOST_TEST(res::pct_adapter::last_decode_peak_bytes() <= res::pct_adapter::decode_budget_bytes(1, 1, 4, bytes.size()));
}

BOOST_AUTO_TEST_CASE(APngWhoseDeflateStreamIsABomb_IsRefusedWithinTheBudget)
{
	// The header says one pixel; the IDAT of a few kilobytes unpacks to a megabyte of zeros.
	constexpr std::size_t repeats = 4096;
	const std::size_t unpacked = 1 + repeats * 258;
	const std::vector<std::byte> bytes = png_1x1(zlib_stream({ 0 }, repeats));
	const std::size_t budget = res::pct_adapter::decode_budget_bytes(1, 1, 4, bytes.size());
	BOOST_REQUIRE(bytes.size() < 16u * 1024u);
	BOOST_REQUIRE(unpacked > budget);
	const res::pct_adapter adapter;

	BOOST_CHECK_EXCEPTION(adapter(res::tag{ "memory://limits/bomb_direct.png" }, bytes), std::runtime_error,
		[](const std::runtime_error& error) { return says(error, "not a decodable image"); });

	BOOST_TEST(res::pct_adapter::last_decode_peak_bytes() > 0u);
	BOOST_TEST(res::pct_adapter::last_decode_peak_bytes() <= budget);

	const auto picture = load<res::picture_resource>("bomb.png", bytes);
	BOOST_TEST(picture.has_error());
}

BOOST_AUTO_TEST_CASE(AJsonBodyLargerThanItsLimit_IsNotParsed)
{
	std::string text = R"({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[],"vertices":[])";
	text.append(4096, ' ');
	text += "}";
	const std::vector<std::byte> bytes = as_bytes(text);

	BOOST_CHECK_EXCEPTION(res::parse_json_body("memory://limits/padded.desc", bytes, 1024), res::load_error,
		[](const res::load_error& error) { return says(error, "is not parsed") && error.what_diagnostic().kind == res::diagnostic_kind::load; });
	// At the limit it is parsed: the size is what was refused, not the text.
	BOOST_TEST(res::parse_json_body("memory://limits/padded.desc", bytes, bytes.size()).contains("__type"));
	BOOST_TEST(res::parse_json_body("memory://limits/padded.desc", bytes).contains("__type"));
}

BOOST_AUTO_TEST_CASE(AGeomDesc_GetsTheLargerLimit_AnyOtherFileTheDefault)
{
	BOOST_TEST(res::max_geometry_json_bytes > res::max_json_bytes);
	BOOST_TEST(res::json_byte_limit("memory://import/robot.geom.desc") == res::max_geometry_json_bytes);
	BOOST_TEST(res::json_byte_limit(res::tag{ "res://models/robot.geom.desc" }.view()) == res::max_geometry_json_bytes);
	BOOST_TEST(res::json_byte_limit("res://materials/robot.mat.desc") == res::max_json_bytes);
	BOOST_TEST(res::json_byte_limit("res://models/robot.desc") == res::max_json_bytes);
	BOOST_TEST(res::json_byte_limit("res://levels/robot.geom.desc.world") == res::max_json_bytes);
}

BOOST_AUTO_TEST_SUITE_END()
