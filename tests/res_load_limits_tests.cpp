#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #928: a small file must not be able to ask for gigabytes: a picture is refused by its header before anything is
// decoded, a desc by its size before anything is parsed.
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

BOOST_AUTO_TEST_CASE(ADescLargerThanTheLimit_IsNotParsed)
{
	std::string text = R"({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[],"vertices":[])";
	text.append(core::desc::desc_adapter_t::max_desc_bytes, ' ');
	text += "}";
	std::vector<std::byte> bytes(text.size());
	std::memcpy(bytes.data(), text.data(), text.size());

	const auto desc = load<desc::desc_base>("padded.desc", std::move(bytes));

	BOOST_TEST(desc.has_error());
	BOOST_TEST(desc.error_text().find("is not parsed") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
