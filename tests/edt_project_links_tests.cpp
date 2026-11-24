#include <boost/test/unit_test.hpp>
#include "edt_project_links.h"
#include "test_folder_link.hpp"
#include "test_reparse_tag.hpp"
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
	test_util::make_folder_link(project.code_dir() / "vendor", dir.path() / "elsewhere");

	const std::vector<edt::problem> problems = edt::code_link_problems(project);

	BOOST_REQUIRE_EQUAL(problems.size(), 1u);
	BOOST_TEST((problems[0].severity == edt::problem_severity::warning));
	BOOST_TEST(problems[0].file == "code/vendor");
	BOOST_TEST(problems[0].message.find("leads outside the project") != std::string::npos);
	BOOST_TEST(problems[0].message.find("elsewhere") != std::string::npos);
	BOOST_TEST(!problems[0].notes.empty());
}

BOOST_AUTO_TEST_CASE(ALinkInCodeIntoSnkOrCache_IsAWarningToo_ALinkToResIsNot)
{
	test_util::temp_dir dir("edt_project_links_private");
	const auto project = open_game(dir);
	fs::create_directories(project.snk_dir());
	fs::create_directories(project.cache_dir());
	fs::create_directories(project.code_dir());
	test_util::make_folder_link(project.code_dir() / "snk_link", project.snk_dir());
	test_util::make_folder_link(project.code_dir() / "cache_link", project.cache_dir());
	test_util::make_folder_link(project.code_dir() / "res_link", project.root / "res");

	const std::vector<edt::problem> problems = edt::code_link_problems(project);

	BOOST_REQUIRE_EQUAL(problems.size(), 2u);
	BOOST_TEST(problems[0].file != "code/res_link");
	BOOST_TEST(problems[1].file != "code/res_link");
}

#ifdef _WIN32
// #1343: a folder of code/ with a third-party reparse tag leads where only its file system filter knows: a warning that
// says so, never no row.
BOOST_AUTO_TEST_CASE(AThirdPartyReparsePointInCode_IsAWarningThatItLeadsToAnUnknownPlace)
{
	test_util::temp_dir dir("edt_project_links_surrogate");
	const auto project = open_game(dir);
	const fs::path folder = project.code_dir() / "tagged";
	fs::create_directories(folder);
	BOOST_REQUIRE_MESSAGE(test_util::set_third_party_surrogate_tag(folder).empty(), "tagged");
	struct remove_on_exit {
		fs::path folder;
		~remove_on_exit() { RemoveDirectoryW(folder.c_str()); }
	} remove{ folder };

	const std::vector<edt::problem> problems = edt::code_link_problems(project);

	BOOST_REQUIRE_EQUAL(problems.size(), 1u);
	BOOST_TEST((problems[0].severity == edt::problem_severity::warning));
	BOOST_TEST(problems[0].file == "code/tagged");
	BOOST_TEST(problems[0].message.find("unknown place") != std::string::npos, problems[0].message);
}
#endif

BOOST_AUTO_TEST_CASE(ACodeFolderWithoutLinks_OrWithoutAnyFolder_HasNoProblems)
{
	test_util::temp_dir dir("edt_project_links_none");
	const auto project = open_game(dir);
	BOOST_TEST(edt::code_link_problems(project).empty());

	dir.write("game/code/src/a.cpp", "x");
	BOOST_TEST(edt::code_link_problems(project).empty());
}

BOOST_AUTO_TEST_SUITE_END()
