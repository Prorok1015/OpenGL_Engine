#include <boost/test/unit_test.hpp>
#include "edt_toolchain.h"
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The pure parts of the game build's toolchain discovery (#434): vswhere and `set` output, PATH handling.

namespace fs = std::filesystem;
using edt::env_flavor;

namespace {

// What cmd /u writes: UTF-16LE code units, byte by byte.
std::string utf16le(std::u16string_view text)
{
	std::string out;
	for (const char16_t unit : text) {
		out += static_cast<char>(unit & 0xFF);
		out += static_cast<char>(unit >> 8);
	}
	return out;
}

std::string utf8(std::u8string_view text)
{
	return std::string(text.begin(), text.end());
}

fs::path unique_temp_dir(std::string_view name)
{
	static int counter = 0;
	return fs::temp_directory_path() / std::format("snk_{}_{}_{}", name,
		std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
}

}

BOOST_AUTO_TEST_SUITE(EdtToolchainTests)

BOOST_AUTO_TEST_CASE(Vswhere_GivesTheInstallationPath)
{
	const auto found = edt::parse_vswhere_output({ "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\r" });
	BOOST_REQUIRE(found.has_value());
	BOOST_CHECK(*found == fs::path("C:\\Program Files\\Microsoft Visual Studio\\2022\\Community"));
}

BOOST_AUTO_TEST_CASE(Vswhere_EmptyOutput_IsNothing)
{
	BOOST_CHECK(!edt::parse_vswhere_output({}).has_value());
	BOOST_CHECK(!edt::parse_vswhere_output({ "", "   " }).has_value());
}

BOOST_AUTO_TEST_CASE(Vswhere_SkipsLogoAndNoise)
{
	const auto found = edt::parse_vswhere_output({
		"Visual Studio Locator version 3.1.7+f39851e70f [query version 3.9.2164.33468]",
		"Copyright (C) Microsoft Corporation. All rights reserved.",
		"",
		"  D:\\VS\\2022\\BuildTools  ",
		"E:\\Other" });
	BOOST_REQUIRE(found.has_value());
	BOOST_CHECK(*found == fs::path("D:\\VS\\2022\\BuildTools"));
}

BOOST_AUTO_TEST_CASE(SetOutput_KeepsVariablesOnly)
{
	const std::vector<std::string> lines = {
		"**********************************************************************",
		"** Visual Studio 2022 Developer Command Prompt v17.14.0",
		"** Copyright (c) 2025 Microsoft Corporation",
		"**********************************************************************",
		"[vcvarsall.bat] Environment initialized for: 'x64'",
		"",
		"=C:=C:\\Users\\dev",
		"=ExitCode=00000000",
		"ALLUSERSPROFILE=C:\\ProgramData",
		"CommonProgramFiles(x86)=C:\\Program Files (x86)\\Common Files",
		"INCLUDE=C:\\VS\\VC\\Tools\\MSVC\\14.44.35207\\include;C:\\Program Files (x86)\\Windows Kits\\10\\include",
		"Path=C:\\VS\\VC\\Tools\\MSVC\\14.44.35207\\bin\\HostX64\\x64;C:\\Windows\\system32\r",
		"VSCMD_ARG_TGT_ARCH=x64",
		"EMPTY=",
		"A=b=c",
	};
	const auto env = edt::parse_set_output(lines);
	BOOST_TEST((env == std::vector<std::string>{
		"ALLUSERSPROFILE=C:\\ProgramData",
		"CommonProgramFiles(x86)=C:\\Program Files (x86)\\Common Files",
		"INCLUDE=C:\\VS\\VC\\Tools\\MSVC\\14.44.35207\\include;C:\\Program Files (x86)\\Windows Kits\\10\\include",
		"Path=C:\\VS\\VC\\Tools\\MSVC\\14.44.35207\\bin\\HostX64\\x64;C:\\Windows\\system32",
		"VSCMD_ARG_TGT_ARCH=x64",
		"EMPTY=",
		"A=b=c" }));
	BOOST_CHECK(edt::variable_of(env, "PATH", env_flavor::windows) == std::optional<std::string>(
		"C:\\VS\\VC\\Tools\\MSVC\\14.44.35207\\bin\\HostX64\\x64;C:\\Windows\\system32"));
	BOOST_CHECK(!edt::variable_of(env, "PATH", env_flavor::posix).has_value());
}

BOOST_AUTO_TEST_CASE(MergePath_PrependsAndKeepsTheNameOnWindows)
{
	std::vector<std::string> env{ "Path=C:\\Windows", "OTHER=1" };
	edt::merge_path(env, fs::path("C:\\VS\\Ninja"), env_flavor::windows);
	BOOST_TEST((env == std::vector<std::string>{ "Path=C:\\VS\\Ninja;C:\\Windows", "OTHER=1" }));
}

BOOST_AUTO_TEST_CASE(MergePath_Posix_IsCaseSensitiveAndAddsPathWhenMissing)
{
	std::vector<std::string> env{ "Path=/not/it", "PATH=/usr/bin" };
	edt::merge_path(env, fs::path("/opt/ninja"), env_flavor::posix);
	BOOST_TEST((env == std::vector<std::string>{ "Path=/not/it", "PATH=/opt/ninja:/usr/bin" }));

	std::vector<std::string> empty;
	edt::merge_path(empty, fs::path("/opt/ninja"), env_flavor::posix);
	BOOST_TEST((empty == std::vector<std::string>{ "PATH=/opt/ninja" }));

	std::vector<std::string> blank{ "PATH=" };
	edt::merge_path(blank, fs::path("/opt/ninja"), env_flavor::posix);
	BOOST_TEST((blank == std::vector<std::string>{ "PATH=/opt/ninja" }));
}

BOOST_AUTO_TEST_CASE(SetVariable_ReplacesOrAppends)
{
	std::vector<std::string> env{ "vslang=1049", "LC_ALL=ru_RU.UTF-8" };
	edt::set_variable(env, "VSLANG", "1033", env_flavor::windows);
	edt::set_variable(env, "LC_ALL", "C", env_flavor::posix);
	edt::set_variable(env, "SNAKE_SDK_DIR", "/sdk", env_flavor::posix);
	BOOST_TEST((env == std::vector<std::string>{ "vslang=1033", "LC_ALL=C", "SNAKE_SDK_DIR=/sdk" }));
}

BOOST_AUTO_TEST_CASE(FindOnPath_LooksInTheGivenPathOnly)
{
	const fs::path dir = unique_temp_dir("edt_toolchain_tests");
	fs::remove_all(dir);
	fs::create_directories(dir / "a");
	fs::create_directories(dir / "b");
	std::ofstream(dir / "b" / "ninja") << "";

	const char separator = edt::k_native_env == env_flavor::windows ? ';' : ':';
	const std::string path_value = (dir / "a").string() + separator + (dir / "b").string();
	const std::vector<std::string> env{ "PATH=" + path_value };
	const auto found = edt::find_on_path(env, "ninja");
	BOOST_REQUIRE(found.has_value());
	BOOST_CHECK(*found == dir / "b" / "ninja");
	BOOST_CHECK(!edt::find_on_path(env, "cmake").has_value());
	BOOST_CHECK(!edt::find_on_path({}, "ninja").has_value());
	fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(SetOutputUtf16_KeepsCyrillicValues)
{
	// A Cyrillic user name: cmd /u gives the values exactly, where the OEM code page would have lost them.
	const std::string bytes = utf16le(u"\uFEFFALLUSERSPROFILE=C:\\ProgramData\r\n"
		u"Path=C:\\VS\\bin;C:\\Users\\\u0418\u0432\u0430\u043d\\tools\r\n"
		u"USERPROFILE=C:\\Users\\\u0418\u0432\u0430\u043d\r\n"
		u"=C:=C:\\\r\n");
	const auto env = edt::parse_set_output_utf16le(bytes);
	BOOST_TEST((env == std::vector<std::string>{
		"ALLUSERSPROFILE=C:\\ProgramData",
		utf8(u8"Path=C:\\VS\\bin;C:\\Users\\\u0418\u0432\u0430\u043d\\tools"),
		utf8(u8"USERPROFILE=C:\\Users\\\u0418\u0432\u0430\u043d") }));
}

BOOST_AUTO_TEST_CASE(SetOutputUtf16_TruncatedAndOddInput)
{
	BOOST_CHECK(edt::utf16le_to_utf8({}).empty());
	BOOST_CHECK(edt::utf16le_to_utf8(std::string(1, 'A')).empty());
	// A last odd byte is dropped.
	BOOST_CHECK_EQUAL(edt::utf16le_to_utf8(utf16le(u"AB") + "C"), "AB");
	// A surrogate pair is one character; one cut off (or unpaired) is U+FFFD.
	BOOST_CHECK_EQUAL(edt::utf16le_to_utf8(utf16le(u"\U0001F600")), utf8(u8"\U0001F600"));
	BOOST_CHECK_EQUAL(edt::utf16le_to_utf8(utf16le(std::u16string(1, char16_t{ 0xD83D }))), utf8(u8"\uFFFD"));
	BOOST_CHECK_EQUAL(edt::utf16le_to_utf8(utf16le(std::u16string{ char16_t{ 0xDE00 }, u'x' })), utf8(u8"\uFFFDx"));
	// Output cut in the middle of a variable: what came whole is kept.
	const std::string bytes = utf16le(u"A=1\r\nB=2\r\nC=");
	const auto env = edt::parse_set_output_utf16le(std::string_view(bytes).substr(0, bytes.size() - 1));
	BOOST_TEST((env == std::vector<std::string>{ "A=1", "B=2" }));
}

BOOST_AUTO_TEST_CASE(SetOutputUtf16_AMultiLineValueDoesNotSpoilTheNextVariables)
{
	const auto env = edt::parse_set_output_utf16le(utf16le(u"A=first\r\nsecond line of A\r\n  third\r\nB=2\r\n"));
	BOOST_TEST((env == std::vector<std::string>{ "A=first", "B=2" }));
}

BOOST_AUTO_TEST_CASE(CmdUnsafePath_RefusesCmdMetacharacters)
{
	BOOST_CHECK(!edt::cmd_unsafe_path_error(fs::path("C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools")).has_value());
	for (const char* bad : { "C:\\VS&calc", "C:\\a|b", "C:\\a<b", "C:\\a>b", "C:\\a^b", "C:\\%TEMP%\\VS", "C:\\VS!x!" }) {
		BOOST_TEST_CONTEXT(bad) {
			const auto error = edt::cmd_unsafe_path_error(fs::path(bad));
			BOOST_REQUIRE(error.has_value());
			BOOST_CHECK(error->find(bad) != std::string::npos);
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
