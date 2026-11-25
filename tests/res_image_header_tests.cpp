#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_base64.h>
#include <snk/resource/res_image_header.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

// #1206: the size of an embedded picture is read from its header alone, before anything decodes it.
namespace {

using res::image_header::picture_format;
using res::image_header::read_picture_size;

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
{
	std::vector<std::byte> out;
	for (const unsigned v : values) out.push_back(static_cast<std::byte>(v));
	return out;
}

std::vector<std::byte> decoded(std::string_view base64)
{
	std::optional<std::vector<std::byte>> bytes = cmd::base64::decode(base64);
	BOOST_REQUIRE(bytes.has_value());
	return *bytes;
}

}

BOOST_AUTO_TEST_SUITE(ResImageHeader)

BOOST_AUTO_TEST_CASE(APng_IsReadFromItsIhdr_AndAHugeOneIsReadWithoutDecoding)
{
	const auto small = read_picture_size(decoded("iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAADUlEQVR4nGNg+M8ARAAHAAH/YKtA0QAAAABJRU5ErkJggg=="));
	BOOST_REQUIRE(small.has_value());
	BOOST_TEST((small->format == picture_format::png));
	BOOST_TEST(small->width == 2u);
	BOOST_TEST(small->height == 1u);

	const auto huge = read_picture_size(decoded("iVBORw0KGgoAAAANSUhEUgABhqAAAYagCAYAAACoUgvIAAAAAElFTkSuQmCC"));
	BOOST_REQUIRE(huge.has_value());
	BOOST_TEST(huge->width == 100000u);
	BOOST_TEST(huge->height == 100000u);
}

BOOST_AUTO_TEST_CASE(ATruncatedOrEmptyHeader_IsNoPicture)
{
	std::vector<std::byte> png = decoded("iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAADUlEQVR4nGNg+M8ARAAHAAH/YKtA0QAAAABJRU5ErkJggg==");
	png.resize(20);
	BOOST_TEST(!read_picture_size(png).has_value());
	BOOST_TEST(!read_picture_size({}).has_value());
	BOOST_TEST(!read_picture_size(decoded("R0lGODlhAQABAAAAADs=")).has_value());	// a GIF
}

BOOST_AUTO_TEST_CASE(AJpeg_IsReadFromItsFrameHeader_AfterTheSegmentsBeforeIt)
{
	// SOI, an APP0 of 4 bytes, a fill byte, SOF0: precision 8, height 3, width 5.
	const auto size = read_picture_size(bytes_of({ 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x4A, 0x46, 0xFF, 0xFF, 0xC0, 0x00, 0x11, 0x08,
		0x00, 0x03, 0x00, 0x05, 0x03 }));
	BOOST_REQUIRE(size.has_value());
	BOOST_TEST((size->format == picture_format::jpeg));
	BOOST_TEST(size->width == 5u);
	BOOST_TEST(size->height == 3u);

	// A scan before any frame header, and a segment longer than the file.
	BOOST_TEST(!read_picture_size(bytes_of({ 0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02 })).has_value());
	BOOST_TEST(!read_picture_size(bytes_of({ 0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xF0, 0x00 })).has_value());
}

BOOST_AUTO_TEST_CASE(ABmp_StoredTopRowFirst_HasThePositiveHeight)
{
	std::vector<std::byte> bmp(54);
	bmp[0] = std::byte{ 'B' };
	bmp[1] = std::byte{ 'M' };
	bmp[14] = std::byte{ 40 };
	bmp[18] = std::byte{ 7 };
	// -2 as a little-endian int32.
	bmp[22] = std::byte{ 0xFE };
	bmp[23] = std::byte{ 0xFF };
	bmp[24] = std::byte{ 0xFF };
	bmp[25] = std::byte{ 0xFF };
	const auto size = read_picture_size(bmp);
	BOOST_REQUIRE(size.has_value());
	BOOST_TEST((size->format == picture_format::bmp));
	BOOST_TEST(size->width == 7u);
	BOOST_TEST(size->height == 2u);
}

BOOST_AUTO_TEST_CASE(ATga_IsTakenOnlyWithAHeaderItsDecoderReads)
{
	const auto size = read_picture_size(decoded("AAACAAAAAAAAAAAAAQABACAIAAD//w=="));
	BOOST_REQUIRE(size.has_value());
	BOOST_TEST((size->format == picture_format::tga));
	BOOST_TEST(size->width == 1u);
	BOOST_TEST(size->height == 1u);

	std::vector<std::byte> tga = decoded("AAACAAAAAAAAAAAAAQABACAIAAD//w==");
	tga[2] = std::byte{ 4 };	// no such image type
	BOOST_TEST(!read_picture_size(tga).has_value());
	tga[2] = std::byte{ 2 };
	tga[16] = std::byte{ 12 };	// no such depth
	BOOST_TEST(!read_picture_size(tga).has_value());
	tga[16] = std::byte{ 8 };	// 8 bits: an index or grey, never true colour
	BOOST_TEST(!read_picture_size(tga).has_value());
	tga[2] = std::byte{ 3 };	// grey of 8 bits
	BOOST_TEST(read_picture_size(tga).has_value());
	tga[16] = std::byte{ 24 };	// grey of 24 bits
	BOOST_TEST(!read_picture_size(tga).has_value());
	tga[2] = std::byte{ 2 };
	tga[16] = std::byte{ 32 };
	tga[1] = std::byte{ 1 };	// a colour map for a true-colour image
	BOOST_TEST(!read_picture_size(tga).has_value());
}

BOOST_AUTO_TEST_CASE(AColourMappedTga_NeedsAColourMapEntrySizeItsDecoderReads)
{
	std::vector<std::byte> tga = decoded("AAACAAAAAAAAAAAAAQABACAIAAD//w==");
	tga[1] = std::byte{ 1 };
	tga[2] = std::byte{ 1 };
	tga[16] = std::byte{ 8 };
	for (const int entry : { 8, 15, 16, 24, 32 }) {
		tga[7] = static_cast<std::byte>(entry);
		BOOST_TEST(read_picture_size(tga).has_value(), entry);
	}
	for (const int entry : { 0, 7, 12, 31, 255 }) {
		tga[7] = static_cast<std::byte>(entry);
		BOOST_TEST(!read_picture_size(tga).has_value(), entry);
	}
}

BOOST_AUTO_TEST_SUITE_END()
