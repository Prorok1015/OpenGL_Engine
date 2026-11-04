#include <boost/test/unit_test.hpp>
#include <snk/project/prj_json_text.h>
#include "edt_build_settings.h"
#include "test_temp_dir.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// Files under <project>/.snk are written without following a link below the project root (#523).

namespace {

namespace fs = std::filesystem;

std::string read_text(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// `link` becomes a link to the folder `target`: a symlink on POSIX, a junction on Windows (no privilege needed).
void make_folder_link(const fs::path& target, const fs::path& link)
{
#ifdef _WIN32
	const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
	BOOST_REQUIRE_MESSAGE(std::system(command.c_str()) == 0 && fs::exists(link), "mklink /J failed for " + link.string());
#else
	fs::create_directory_symlink(target, link);
#endif
}

// `link` becomes a symlink to the file `target`; false (with the reason in the test log) when the system refuses it:
// Windows without developer mode or the symlink privilege.
bool make_file_link(const fs::path& target, const fs::path& link)
{
	std::error_code ec;
	fs::create_symlink(target, link, ec);
#ifdef _WIN32
	if (ec) {
		BOOST_TEST_MESSAGE("file symlink not created (" + ec.message() + "): no developer mode or symlink privilege; that case is not run");
		return false;
	}
#endif
	BOOST_REQUIRE_MESSAGE(!ec, "cannot create a symlink: " + ec.message());
	return true;
}

constexpr std::string_view k_victim = R"({"victim":true})";

}

BOOST_AUTO_TEST_SUITE(PrjWriteProjectFileTests)

BOOST_AUTO_TEST_CASE(Write_CreatesTheFolderAndReplacesTheFile)
{
	test_util::temp_dir dir("prj_write_project_file_plain");
	BOOST_REQUIRE(!prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "one").has_value());
	BOOST_TEST(fs::is_directory(fs::symlink_status(dir.path() / ".snk")));
	BOOST_TEST(read_text(dir.path() / ".snk" / "build.json") == "one");

	BOOST_REQUIRE(!prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "two").has_value());
	BOOST_TEST(read_text(dir.path() / ".snk" / "build.json") == "two");
	const auto entries = std::distance(fs::directory_iterator(dir.path() / ".snk"), fs::directory_iterator{});
	BOOST_TEST(entries == 1); // no temp file left
}

BOOST_AUTO_TEST_CASE(Write_RefusesAPathThatLeavesTheRoot)
{
	test_util::temp_dir dir("prj_write_project_file_escape");
	BOOST_TEST(prj::write_project_file(dir.path(), fs::path("..") / "escape.json", "x").has_value());
	BOOST_TEST(!fs::exists(dir.path().parent_path() / "escape.json"));
}

BOOST_AUTO_TEST_CASE(Write_RefusesALinkAtTheFile_AndLeavesItsTargetAlone)
{
	test_util::temp_dir dir("prj_write_project_file_file_link");
	const fs::path victim = dir.write("victim.json", k_victim);
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(victim, dir.path() / ".snk" / "build.json")) return;

	BOOST_TEST(prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "written").has_value());
	BOOST_TEST(read_text(victim) == k_victim);
	BOOST_TEST(fs::is_symlink(fs::symlink_status(dir.path() / ".snk" / "build.json")));
}

BOOST_AUTO_TEST_CASE(Write_RefusesALinkAtTheFolder_AndWritesNothingIntoItsTarget)
{
	test_util::temp_dir dir("prj_write_project_file_folder_link");
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	make_folder_link(outside, dir.path() / ".snk");

	BOOST_TEST(prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "written").has_value());
	BOOST_TEST(fs::is_empty(outside));
}

BOOST_AUTO_TEST_CASE(UpdateBuildSettings_RefusesALinkAtBuildJson)
{
	test_util::temp_dir dir("prj_write_project_file_settings_file_link");
	const fs::path victim = dir.write("victim.json", k_victim);
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(victim, dir.path() / ".snk" / "build.json")) return;

	const auto problem = edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	});
	BOOST_TEST(problem.has_value());
	BOOST_TEST(read_text(victim) == k_victim);
}

BOOST_AUTO_TEST_CASE(UpdateBuildSettings_RefusesALinkAtSnk)
{
	test_util::temp_dir dir("prj_write_project_file_settings_folder_link");
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	make_folder_link(outside, dir.path() / ".snk");

	const auto problem = edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	});
	BOOST_TEST(problem.has_value());
	BOOST_TEST(fs::is_empty(outside));
}

BOOST_AUTO_TEST_CASE(UpdateBuildSettings_WritesThroughTheRealWriter)
{
	test_util::temp_dir dir("prj_write_project_file_settings_plain");
	BOOST_REQUIRE(!edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	}).has_value());
	BOOST_TEST(edt::load_build_settings(dir.path() / ".snk").live_enabled);
}

BOOST_AUTO_TEST_SUITE_END()
