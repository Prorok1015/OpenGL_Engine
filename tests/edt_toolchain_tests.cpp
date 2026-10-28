#include <boost/test/unit_test.hpp>
#include "edt_toolchain.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// The pure parts of the game build's toolchain discovery (#434): vswhere and `set` output, PATH handling.

namespace fs = std::filesystem;
using edt::env_flavor;

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
	const fs::path dir = fs::temp_directory_path() / "snk_edt_toolchain_tests";
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

BOOST_AUTO_TEST_SUITE_END()
