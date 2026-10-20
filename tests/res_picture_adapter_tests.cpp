#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/res_system.h>
#include <cstring>
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

BOOST_FIXTURE_TEST_CASE(UndecodablePicture_EndsInAnErrorNotInAReadyNull, test_util::ram_res_env)
{
	rs->registrate_adapter<res::pct_adapter>(res::pct_adapter::INFO);
	const auto tag = put("bad.png", "this is not a picture");

	auto handle = rs->require<res::picture_resource>(tag);
	pump();

	BOOST_TEST(handle.has_error());
	BOOST_TEST(!handle.is_ready());
}

BOOST_AUTO_TEST_SUITE_END()
