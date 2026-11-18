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

// #1160: the adapter saves a picture as JPEG in memory and reads it back; a flat colour survives the lossy codec.
BOOST_AUTO_TEST_CASE(Pct_SavedPicture_LoadsBackWithItsSizeAndColour)
{
	constexpr int width = 16;
	constexpr int height = 8;
	constexpr int channels = 3;
	auto* pixels = new unsigned char[width * height * channels];
	for (int i = 0; i < width * height; ++i) {
		pixels[i * channels + 0] = 200;
		pixels[i * channels + 1] = 100;
		pixels[i * channels + 2] = 50;
	}
	const res::tag tag{ "memory://t/flat.jpg" };
	const auto saved = std::make_shared<res::picture_resource>(tag, glm::ivec2{ width, height }, channels, pixels);

	res::pct_adapter adapter;
	const std::vector<std::byte> jpeg = adapter.serialize(tag, saved);
	BOOST_REQUIRE(!jpeg.empty());

	const auto loaded = adapter(tag, jpeg);
	BOOST_REQUIRE(loaded);
	BOOST_TEST(loaded->size().x == width);
	BOOST_TEST(loaded->size().y == height);
	BOOST_REQUIRE(loaded->channels() == channels);
	const unsigned char* centre = loaded->data() + ((height / 2) * width + width / 2) * channels;
	BOOST_TEST(std::abs(centre[0] - 200) <= 4);
	BOOST_TEST(std::abs(centre[1] - 100) <= 4);
	BOOST_TEST(std::abs(centre[2] - 50) <= 4);
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
