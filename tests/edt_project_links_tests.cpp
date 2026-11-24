#include <boost/test/unit_test.hpp>
#include "edt_project_links.h"
#include "test_temp_dir.hpp"
#include <snk/project/prj_project.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

prj::project_context open_game(test_util::temp_dir& dir)
{
	dir.write("game/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	fs::create_directories(dir.path() / "game" / "res");
	const auto opened = prj::open_project(dir.path() / "game");
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	return *opened.context;
}

}

BOOST_AUTO_TEST_SUITE(EdtProjectLinks)

BOOST_AUTO_TEST_CASE(ALinkInCodeThatLeavesTheProject_IsAWarningNamingTheLinkAndItsTarget)
{
	test_util::temp_dir dir("edt_project_links");
	const auto project = open_game(dir);
	fs::create_directories(project.code_dir() / "src");
	fs::create_directories(dir.path() / "elsewhere");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "elsewhere", project.code_dir() / "vendor", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	const std::vector<edt::problem> problems = edt::code_link_problems(project);

	BOOST_REQUIRE_EQUAL(problems.size(), 1u);
	BOOST_TEST(problems[0].severity == edt::problem_severity::warning);
	BOOST_TEST(problems[0].file == "code/vendor");
	BOOST_TEST(problems[0].message.find("leads outside the project") != std::string::npos);
	BOOST_TEST(problems[0].message.find("elsewhere") != std::string::npos);
	BOOST_TEST(!problems[0].notes.empty());
}

BOOST_AUTO_TEST_CASE(ACodeFolderWithoutLinks_OrWithoutAnyFolder_HasNoProblems)
{
	test_util::temp_dir dir("edt_project_links_none");
	const auto project = open_game(dir);
	BOOST_TEST(edt::code_link_problems(project).empty());

	dir.write("game/code/src/a.cpp", "x");
	BOOST_TEST(edt::code_link_problems(project).empty());
}

BOOST_AUTO_TEST_SUITE_END()
