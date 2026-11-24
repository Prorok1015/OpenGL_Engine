#include <boost/test/unit_test.hpp>
#include "edt_shader_text.h"
#include <cstddef>
#include <span>
#include <string>

namespace {

using namespace edt;

std::optional<shader_text_file> read(const std::string& text, std::string& error)
{
	return read_shader_text(std::as_bytes(std::span<const char>(text.data(), text.size())), error);
}

}

BOOST_AUTO_TEST_SUITE(EdtShaderText)

BOOST_AUTO_TEST_CASE(PlainLfText_IsTakenAsIs_AndWrittenBackTheSame)
{
	std::string error;
	const std::string source = "#version 450\nvoid main() {}\n";
	const auto file = read(source, error);
	BOOST_REQUIRE_MESSAGE(file.has_value(), error);
	BOOST_TEST(file->text == source);
	BOOST_TEST((file->ending == line_ending::lf));
	BOOST_TEST(!file->bom);
	BOOST_TEST(write_shader_text(*file) == source);
}

BOOST_AUTO_TEST_CASE(CrlfAndABom_AreNotedAndComeBack)
{
	std::string error;
	const std::string source = "\xEF\xBB\xBF#version 450\r\nvoid main() {}\r\n";
	const auto file = read(source, error);
	BOOST_REQUIRE_MESSAGE(file.has_value(), error);
	BOOST_TEST(file->text == "#version 450\nvoid main() {}\n");
	BOOST_TEST((file->ending == line_ending::crlf));
	BOOST_TEST(file->bom);
	BOOST_TEST(!file->mixed_endings);
	BOOST_TEST(write_shader_text(*file) == source);
}

BOOST_AUTO_TEST_CASE(MixedEndings_TakeTheDominantOne_AndSayTheyWereMixed)
{
	std::string error;
	const auto file = read("a\r\nb\r\nc\nd", error);
	BOOST_REQUIRE_MESSAGE(file.has_value(), error);
	BOOST_TEST((file->ending == line_ending::crlf));
	BOOST_TEST(file->mixed_endings);
	BOOST_TEST(write_shader_text(*file) == "a\r\nb\r\nc\r\nd");
}

BOOST_AUTO_TEST_CASE(ALoneCarriageReturn_IsKeptInTheText)
{
	std::string error;
	const auto file = read("a\rb\n", error);
	BOOST_REQUIRE_MESSAGE(file.has_value(), error);
	BOOST_TEST(file->text == "a\rb\n");
}

BOOST_AUTO_TEST_CASE(ABinaryFile_AnInvalidOneAndAHugeOne_AreRefusedWithAReason)
{
	std::string error;
	BOOST_TEST(!read(std::string("ab\0cd", 5), error).has_value());
	BOOST_TEST(error.find("NUL") != std::string::npos);

	error.clear();
	BOOST_TEST(!read("caf\xE9", error).has_value());
	BOOST_TEST(error.find("UTF-8") != std::string::npos);

	error.clear();
	BOOST_TEST(!read(std::string(k_max_shader_text_bytes + 1, 'a'), error).has_value());
	BOOST_TEST(error.find("4 MiB") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Utf8_AcceptsRealText_AndRefusesTheBrokenForms)
{
	BOOST_TEST(is_valid_utf8("plain"));
	BOOST_TEST(is_valid_utf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
	BOOST_TEST(!is_valid_utf8("\xC3"));
	BOOST_TEST(!is_valid_utf8("\xC0\xAF"));
	BOOST_TEST(!is_valid_utf8("\xED\xA0\x80"));
	BOOST_TEST(!is_valid_utf8("\xF4\x90\x80\x80"));
	BOOST_TEST(!is_valid_utf8("a\x80"));
	BOOST_TEST(!is_valid_utf8("\xE2\x82"));
}

BOOST_AUTO_TEST_CASE(WithLineEnding_Lf_IsTheIdentity_Crlf_DoublesEveryBreak)
{
	BOOST_TEST(with_line_ending("a\nb\n", line_ending::lf) == "a\nb\n");
	BOOST_TEST(with_line_ending("a\nb\n", line_ending::crlf) == "a\r\nb\r\n");
	BOOST_TEST(with_line_ending("", line_ending::crlf).empty());
}

BOOST_AUTO_TEST_SUITE_END()
