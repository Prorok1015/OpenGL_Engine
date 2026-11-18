#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// A picture that cannot be decoded ends in an error, never in a ready resource without data.

namespace {

std::vector<std::byte> bytes_of(const std::string_view text)
{
	std::vector<std::byte> bytes(text.size());
	std::memcpy(bytes.data(), text.data(), text.size());
	return bytes;
}

}

BOOST_AUTO_TEST_SUITE(ResPictureAdapterTests)

BOOST_AUTO_TEST_CASE(Pct_UndecodableData_Throws)
{
	res::pct_adapter adapter;
	BOOST_CHECK_THROW(adapter(res::tag{ "memory://t/bad.png" }, bytes_of("this is not a picture")), std::runtime_error);
	BOOST_CHECK_THROW(adapter(res::tag{ "memory://t/empty.png" }, {}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(Raw_ShortOrMismatchedData_Throws)
{
	res::raw_image_adapter adapter;
	BOOST_CHECK_THROW(adapter(res::tag{ "memory://t/short.raw" }, bytes_of("abc")), std::runtime_error);

	res::raw_image_adapter::raw_image_header header{ { 4, 4 }, 4 }; // wants 64 bytes of pixels
	std::vector<std::byte> data(res::raw_image_adapter::HEADER_SIZE + 8);
	std::memcpy(data.data(), &header, sizeof(header));
	BOOST_CHECK_THROW(adapter(res::tag{ "memory://t/cut.raw" }, data), std::runtime_error);

	header.size = { 1 << 30, 1 << 30 };
	std::memcpy(data.data(), &header, sizeof(header));
	BOOST_CHECK_THROW(adapter(res::tag{ "memory://t/huge.raw" }, data), std::runtime_error);
}

// #1160: the adapter saves a picture as JPEG in memory and reads it back; flat colours survive the lossy codec.
// The rows come back reversed: serialize writes the bottom-up rows the adapter loads as they are (#1222).
BOOST_AUTO_TEST_CASE(Pct_SavedPicture_LoadsBackWithItsRowsReversed)
{
	constexpr int width = 16;
	constexpr int height = 16;
	constexpr int channels = 3;
	constexpr unsigned char first_rows[channels] = { 200, 100, 50 };
	constexpr unsigned char last_rows[channels] = { 40, 90, 220 };
	auto* pixels = new unsigned char[width * height * channels];
	for (int y = 0; y < height; ++y) {
		const unsigned char* colour = y < height / 2 ? first_rows : last_rows;
		for (int x = 0; x < width; ++x) {
			std::memcpy(pixels + (y * width + x) * channels, colour, channels);
		}
	}
	const res::tag tag{ "memory://t/halves.jpg" };
	const auto saved = std::make_shared<res::picture_resource>(tag, glm::ivec2{ width, height }, channels, pixels);

	res::pct_adapter adapter;
	const std::vector<std::byte> jpeg = adapter.serialize(tag, saved);
	BOOST_REQUIRE(!jpeg.empty());

	const auto loaded = adapter(tag, jpeg);
	BOOST_REQUIRE(loaded);
	BOOST_TEST(loaded->size().x == width);
	BOOST_TEST(loaded->size().y == height);
	BOOST_REQUIRE(loaded->channels() == channels);
	const auto has_colour = [&](int y, const unsigned char* colour) {
		const unsigned char* pixel = loaded->data() + (y * width + width / 2) * channels;
		for (int c = 0; c < channels; ++c) {
			if (std::abs(pixel[c] - colour[c]) > 6) {
				return false;
			}
		}
		return true;
	};
	BOOST_TEST(has_colour(height / 4, last_rows));
	BOOST_TEST(has_colour(height * 3 / 4, first_rows));
}

BOOST_FIXTURE_TEST_CASE(UndecodablePicture_EndsInAnErrorNotInAReadyNull, test_util::ram_res_env)
{
	rs->registrate_adapter<res::pct_adapter>(res::pct_adapter::INFO);
	const auto tag = put("bad.png", "this is not a picture");

	auto handle = rs->require<res::picture_resource>(tag);
	pump();

	BOOST_TEST(handle.has_error());
	BOOST_TEST(!handle.is_ready());
	BOOST_TEST(handle.get_control_block()->error_msg.find("Adapter error") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
