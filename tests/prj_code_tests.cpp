#include <boost/test/unit_test.hpp>
#include <snk/project/prj_code.h>
#include <snk/project/prj_link_check.h>
#include <snk/project/prj_project.h>
#include <snk/resource/resolvers/res_write_policy.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// What counts as code in code:// (#450): categories, ignored folders, the write patterns and the project's code/ folder.
namespace {

namespace fs = std::filesystem;

res::write_policy code_policy()
{
	return res::write_policy{ "code", prj::code_write_patterns(), false };
}

prj::project_context open_game(test_util::temp_dir& dir)
{
	dir.write("game/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	fs::create_directories(dir.path() / "game" / "res");
	const auto opened = prj::open_project(dir.path() / "game");
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	return *opened.context;
}

}

BOOST_AUTO_TEST_SUITE(PrjCode)

BOOST_AUTO_TEST_CASE(CategoryOf_SortsByTheLastSegment)
{
	using prj::code_category;
	BOOST_TEST((prj::code_category_of("src/game.cpp") == code_category::source));
	BOOST_TEST((prj::code_category_of("src/game.CC") == code_category::source));
	BOOST_TEST((prj::code_category_of("include/game.h") == code_category::header));
	BOOST_TEST((prj::code_category_of("include\\game.HPP") == code_category::header));
	BOOST_TEST((prj::code_category_of("a/b.inl") == code_category::header));
	BOOST_TEST((prj::code_category_of("CMakeLists.txt") == code_category::cmake));
	BOOST_TEST((prj::code_category_of("src/cmakelists.TXT") == code_category::cmake));
	BOOST_TEST((prj::code_category_of("cmake/toolchain.cmake") == code_category::cmake));
	BOOST_TEST((prj::code_category_of("CMakePresets.json") == code_category::cmake));
	BOOST_TEST((prj::code_category_of("CMakeUserPresets.json") == code_category::cmake));
}

BOOST_AUTO_TEST_CASE(CategoryOf_EverythingElseIsOther)
{
	using prj::code_category;
	BOOST_TEST((prj::code_category_of("readme.md") == code_category::other));
	BOOST_TEST((prj::code_category_of("data.json") == code_category::other));
	BOOST_TEST((prj::code_category_of("notes.txt") == code_category::other));
	BOOST_TEST((prj::code_category_of("build/game.cpp.o") == code_category::other));
	BOOST_TEST((prj::code_category_of(".h") == code_category::other));
	BOOST_TEST((prj::code_category_of("") == code_category::other));
	BOOST_TEST(!prj::is_code_file("src/readme.md"));
	BOOST_TEST(prj::is_code_file("src/a.cpp"));
}

BOOST_AUTO_TEST_CASE(IgnoredDirs_AreTheListedNamesAndHiddenOnes)
{
	for (const auto name : prj::code_ignored_dir_names()) {
		BOOST_TEST_INFO(name);
		BOOST_TEST(prj::is_code_ignored_dir(name));
	}
	BOOST_TEST(prj::is_code_ignored_dir("Build"));
	BOOST_TEST(prj::is_code_ignored_dir(".git"));
	BOOST_TEST(prj::is_code_ignored_dir(".cache"));
	BOOST_TEST(prj::is_code_ignored_dir(".snk"));
	BOOST_TEST(!prj::is_code_ignored_dir("src"));
	BOOST_TEST(!prj::is_code_ignored_dir(""));
}

BOOST_AUTO_TEST_CASE(WritePatterns_AllowCodeFilesAnywhereInCode)
{
	const auto policy = code_policy();
	BOOST_TEST(policy.allows("game.cpp"));
	BOOST_TEST(policy.allows("src/game/player.cpp"));
	BOOST_TEST(policy.allows("include/player.h"));
	BOOST_TEST(policy.allows("include/player.hpp"));
	BOOST_TEST(policy.allows("include/player.inl"));
	BOOST_TEST(policy.allows("CMakeLists.txt"));
	BOOST_TEST(policy.allows("src/CMakeLists.txt"));
	BOOST_TEST(policy.allows("cmake/flags.cmake"));
	BOOST_TEST(policy.allows("CMakePresets.json"));
	BOOST_TEST(policy.allows("CMakeUserPresets.json"));
	BOOST_TEST(policy.allows("SRC/PLAYER.CPP"));
}

BOOST_AUTO_TEST_CASE(WritePatterns_RefuseOtherFiles)
{
	const auto policy = code_policy();
	BOOST_TEST(!policy.allows("readme.md"));
	BOOST_TEST(!policy.allows("src/data.bin"));
	BOOST_TEST(!policy.allows("src/player.cpp.exe"));
	BOOST_TEST(!policy.allows("src/script.sh"));
	BOOST_TEST(!policy.allows("src"));
}

BOOST_AUTO_TEST_CASE(WritePatterns_RefuseIgnoredFolders_AtAnyDepthAndInAnyCase)
{
	const auto policy = code_policy();
	for (const std::string folder : { "build", "Build", "BUILD", "out", "_deps", "CMakeFiles", ".cache", ".git", ".snk", ".vs" }) {
		BOOST_TEST_INFO(folder);
		BOOST_TEST(!policy.allows(folder + "/a.cpp"));
		BOOST_TEST(!policy.allows(folder + "/deep/er/a.h"));
		BOOST_TEST(!policy.allows("src/" + folder + "/a.cpp"));
	}
	BOOST_TEST(!policy.allows("build/CMakeLists.txt"));
	BOOST_TEST(!policy.allows("src/_deps/lib/CMakeLists.txt"));
	BOOST_TEST(policy.allows("src/building/a.cpp"));
	BOOST_TEST(policy.allows("src/build_tools/a.cpp"));
}

BOOST_AUTO_TEST_CASE(WatcherIgnoreListAndWriteDenies_DoNotDrift)
{
	const auto policy = code_policy();
	for (const auto name : prj::code_ignored_dir_names()) {
		BOOST_TEST_INFO(name);
		BOOST_TEST(!policy.allows(std::string{ name } + "/a.cpp"));
	}
	BOOST_TEST(!policy.allows(".anything/a.cpp"));
}

BOOST_AUTO_TEST_CASE(CodeDir_IsUnderTheProjectRoot)
{
	test_util::temp_dir dir("prj_code_dir");
	const auto project = open_game(dir);
	BOOST_TEST((project.code_dir() == project.root / "code"));
}

BOOST_AUTO_TEST_CASE(Verify_AcceptsAMissingOrARealCodeFolder)
{
	test_util::temp_dir dir("prj_code_verify_ok");
	const auto project = open_game(dir);
	BOOST_TEST(!prj::verify_project_mounts(project).has_value());

	fs::create_directories(project.code_dir() / "src");
	BOOST_TEST(!prj::verify_project_mounts(project).has_value());
}

BOOST_AUTO_TEST_CASE(Verify_RefusesACodeFolderThatIsAFile)
{
	test_util::temp_dir dir("prj_code_verify_file");
	const auto project = open_game(dir);
	dir.write("game/code", "not a folder");

	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("not a directory") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Verify_RefusesASymlinkedCodeFolder)
{
	test_util::temp_dir dir("prj_code_verify_link");
	const auto project = open_game(dir);
	fs::create_directories(dir.path() / "elsewhere");

	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "elsewhere", project.code_dir(), ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("link") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Verify_RefusesACodeFolderInsideAContentRoot)
{
	test_util::temp_dir dir("prj_code_verify_overlap");
	const auto project = open_game(dir);
	fs::create_directories(project.code_dir() / "assets");

	for (const fs::path& root : { project.code_dir(), project.code_dir() / "assets" }) {
		BOOST_TEST_INFO(root.string());
		prj::project_context overlapping = project;
		overlapping.content_roots = { root };

		// Refused by the rule of #911 (code is the project's own folder) before the overlap with the code folder is looked at.
		const auto problem = prj::verify_project_mounts(overlapping);
		BOOST_REQUIRE(problem.has_value());
		BOOST_TEST(problem->find("the project's own folder") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(Verify_RefusesALinkToAFolderInsideTheProjectToo)
{
	test_util::temp_dir dir("prj_code_verify_inner_link");
	const auto project = open_game(dir);
	fs::create_directories(project.root / "inner");

	std::error_code ec;
	fs::create_directory_symlink(project.root / "inner", project.code_dir(), ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("is a link") != std::string::npos);
}

#ifdef _WIN32
BOOST_AUTO_TEST_CASE(Verify_RefusesAJunctionAsTheCodeFolder)
{
	test_util::temp_dir dir("prj_code_verify_junction");
	const auto project = open_game(dir);
	fs::create_directories(project.root / "inner");

	const std::string command = "mklink /J \"" + project.code_dir().string() + "\" \"" + (project.root / "inner").string() + "\" >nul";
	if (std::system(("cmd /c " + command).c_str()) != 0 || !fs::exists(project.code_dir())) {
		BOOST_TEST_MESSAGE("junctions unavailable");
		return;
	}
	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("is a link") != std::string::npos);
}
#endif

// #1241: a link anywhere in .cache, shipped with the project, would take the cache's plain file writes along.
BOOST_AUTO_TEST_CASE(Verify_AcceptsARealCacheWithFoldersAndFiles)
{
	test_util::temp_dir dir("prj_cache_real");
	const auto project = open_game(dir);
	fs::create_directories(project.cache_dir() / "asset_ops");
	fs::create_directories(project.cache_dir() / "modules" / "1-0");
	dir.write("game/.cache/asset_ops/journal.json", "{}");

	BOOST_TEST(!prj::verify_project_mounts(project).has_value());
}

BOOST_AUTO_TEST_CASE(Verify_RefusesASymlinkedCache)
{
	test_util::temp_dir dir("prj_cache_link");
	const auto project = open_game(dir);
	fs::create_directories(dir.path() / "elsewhere");

	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "elsewhere", project.cache_dir(), ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("is a link") != std::string::npos);
	BOOST_TEST(problem->find("remove it") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Verify_RefusesALinkInsideTheCacheThatLeadsIntoTheProject)
{
	test_util::temp_dir dir("prj_cache_inner_link");
	const auto project = open_game(dir);
	fs::create_directories(project.cache_dir());

	std::error_code ec;
	fs::create_directory_symlink(project.root / "res", project.cache_dir() / "asset_ops", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("asset_ops") != std::string::npos);
	BOOST_TEST(problem->find("remove it and open the project again") != std::string::npos);
	BOOST_TEST(fs::exists(project.cache_dir() / "asset_ops"), "nothing is removed by the check");
}

BOOST_AUTO_TEST_CASE(Verify_RefusesAFileLinkDeepInTheCache)
{
	test_util::temp_dir dir("prj_cache_file_link");
	const auto project = open_game(dir);
	fs::create_directories(project.memory_dir() / "models");
	dir.write("game/res/target.desc", "{}");

	std::error_code ec;
	fs::create_symlink(project.root / "res" / "target.desc", project.memory_dir() / "models" / "link.desc", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto problem = prj::verify_project_mounts(project);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("link.desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LinkBelow_RefusesAWalkPastItsLimit)
{
	test_util::temp_dir dir("prj_cache_limit");
	for (int i = 0; i < 5; ++i) {
		dir.write("cache/f" + std::to_string(i), "x");
	}

	const auto problem = prj::link_below(dir.path() / "cache", 3);
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("more than 3 entries") != std::string::npos);
	BOOST_TEST(!prj::link_below(dir.path() / "cache", 5).has_value());
	BOOST_TEST(!prj::link_below(dir.path() / "missing").has_value());
}

#ifdef _WIN32
BOOST_AUTO_TEST_CASE(Verify_RefusesAJunctionAsTheCacheOrInsideIt)
{
	for (const bool inside : { false, true }) {
		BOOST_TEST_INFO("junction inside .cache: " << inside);
		test_util::temp_dir dir(inside ? "prj_cache_junction_in" : "prj_cache_junction");
		const auto project = open_game(dir);
		fs::create_directories(project.root / "inner");
		if (inside) {
			fs::create_directories(project.cache_dir());
		}
		const fs::path link = inside ? project.cache_dir() / "modules" : project.cache_dir();

		const std::string target = (project.root / "inner").string();
		const std::string command = "mklink /J \"" + link.string() + "\" \"" + target + "\" >nul";
		if (std::system(("cmd /c " + command).c_str()) != 0 || !fs::exists(link)) {
			BOOST_TEST_MESSAGE("junctions unavailable");
			return;
		}
		const auto problem = prj::verify_project_mounts(project);
		BOOST_REQUIRE(problem.has_value());
		BOOST_TEST(problem->find("is a link") != std::string::npos);
	}
}
#endif

BOOST_AUTO_TEST_SUITE_END()
