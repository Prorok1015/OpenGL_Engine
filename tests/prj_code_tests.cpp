#include <boost/test/unit_test.hpp>
#include <snk/project/prj_code.h>
#include <snk/project/prj_link_check.h>
#include "test_folder_link.hpp"
#include <snk/project/prj_project.h>
#include <snk/resource/resolvers/res_write_policy.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

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

BOOST_AUTO_TEST_CASE(LinkBelow_RefusesALinkInsideTheCacheThatLeadsIntoTheProject)
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
	// The cheap check of the folders themselves leaves the depth to link_below, walked once by the resource init.
	BOOST_TEST(!prj::verify_project_mounts(project).has_value());
	const auto problem = prj::link_below(project.cache_dir());
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("asset_ops") != std::string::npos);
	BOOST_TEST(problem->find("remove it and open the project again") != std::string::npos);
	BOOST_TEST(fs::exists(project.cache_dir() / "asset_ops"), "nothing is removed by the check");
}

BOOST_AUTO_TEST_CASE(LinkBelow_RefusesAFileLinkDeepInTheCache)
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
	const auto problem = prj::link_below(project.cache_dir());
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("link.desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(IsMigrationBackupFolder_IsExactlyMigrationsIdBackup)
{
	BOOST_TEST(prj::is_migration_backup_folder("migrations/20260101/backup"));
	BOOST_TEST(!prj::is_migration_backup_folder("migrations/20260101/backup/res"));
	BOOST_TEST(!prj::is_migration_backup_folder("migrations/backup"));
	BOOST_TEST(!prj::is_migration_backup_folder("migrations//backup"));
	BOOST_TEST(!prj::is_migration_backup_folder("migrations/a/b/backup"));
	BOOST_TEST(!prj::is_migration_backup_folder("mcp/20260101/backup"));
	BOOST_TEST(!prj::is_migration_backup_folder("migrations/20260101"));
}

prj::link_walk_options snk_walk()
{
	prj::link_walk_options options;
	options.what = "the project's .snk folder";
	options.skip_folder = prj::is_migration_backup_folder;
	return options;
}

BOOST_AUTO_TEST_CASE(LinkBelow_SnkWalk_RefusesALinkAnywhereInSnk_ExceptInsideAMigrationBackup)
{
	test_util::temp_dir dir("prj_snk_links");
	const auto project = open_game(dir);
	const fs::path backup = project.snk_dir() / "migrations" / "m1" / "backup";
	fs::create_directories(backup / "res");
	fs::create_directories(project.snk_dir() / "mcp");
	dir.write("game/.snk/migrations/m1/report.json", "{}");

	// A folder link (a junction on Windows) inside a backup is not walked.
	test_util::make_folder_link(backup / "res" / "inner", project.root / "res");
	BOOST_TEST(!prj::link_below(project.snk_dir(), snk_walk()).has_value(), "inside a backup nothing is walked");

	test_util::make_folder_link(project.snk_dir() / "mcp" / "deep", project.root / "res");
	const auto deep = prj::link_below(project.snk_dir(), snk_walk());
	BOOST_REQUIRE(deep.has_value());
	BOOST_TEST(deep->find("deep") != std::string::npos);
	BOOST_TEST(deep->find("the project's .snk folder") != std::string::npos);
	BOOST_TEST(deep->find("remove it and open the project again") != std::string::npos);
	fs::remove(project.snk_dir() / "mcp" / "deep");

#ifndef _WIN32
	// A file link needs a privilege on Windows: the case runs where it can be made.
	dir.write("game/res/target.desc", "{}");
	std::error_code ec;
	fs::create_symlink(project.root / "res" / "target.desc", project.snk_dir() / "migrations" / "m1" / "journal.json", ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());
	const auto journal = prj::link_below(project.snk_dir(), snk_walk());
	BOOST_REQUIRE(journal.has_value());
	BOOST_TEST(journal->find("journal.json") != std::string::npos);
	fs::remove(project.snk_dir() / "migrations" / "m1" / "journal.json");
#endif

	fs::remove_all(backup);
	test_util::make_folder_link(backup, project.root / "res");
	const auto backup_itself = prj::link_below(project.snk_dir(), snk_walk());
	BOOST_REQUIRE_MESSAGE(backup_itself.has_value(), "a link AT the backup folder is still refused");
	BOOST_TEST(backup_itself->find("backup") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LinkBelow_SnkWalk_RefusesAWalkPastItsLimit_SayingWhatTheFolderIs)
{
	test_util::temp_dir dir("prj_snk_limit");
	for (int i = 0; i < 6; ++i) {
		dir.write("snk/f" + std::to_string(i), "x");
	}
	prj::link_walk_options options = snk_walk();
	options.max_entries = 3;

	const auto problem = prj::link_below(dir.path() / "snk", options);

	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("more than 3 entries") != std::string::npos);
	BOOST_TEST(problem->find("the project's .snk folder") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LinksLeavingProject_ReportsOnlyTheLinksThatLeaveIt)
{
	test_util::temp_dir dir("prj_code_leaving");
	const auto project = open_game(dir);
	fs::create_directories(project.code_dir() / "src");
	fs::create_directories(dir.path() / "elsewhere");
	test_util::make_folder_link(project.code_dir() / "vendor", dir.path() / "elsewhere");
	test_util::make_folder_link(project.code_dir() / "inner", project.code_dir() / "src");
	std::vector<std::string> expected{ "vendor" };
#ifndef _WIN32
	dir.write("game/code/src/inside.cpp", "x");
	std::error_code ec;
	fs::create_symlink(project.code_dir() / "src" / "inside.cpp", project.code_dir() / "src" / "stays_in.cpp", ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());
	fs::create_symlink(dir.path() / "gone.cpp", project.code_dir() / "src" / "dangling.cpp", ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());
	expected.push_back("dangling.cpp");
#endif

	const prj::leaving_links found = prj::links_leaving_project(project.code_dir(), project.root);

	BOOST_TEST(found.complete);
	std::vector<std::string> names;
	for (const prj::leaving_link& link : found.found) names.push_back(link.link.filename().string());
	std::ranges::sort(names);
	std::ranges::sort(expected);
	BOOST_TEST((names == expected));
	BOOST_TEST(fs::exists(project.code_dir() / "vendor"), "nothing is removed by the check");
}

BOOST_AUTO_TEST_CASE(LinksLeavingProject_ALinkIntoSnkOrCache_CountsAsLeavingWhenTheyAreNamed)
{
	test_util::temp_dir dir("prj_code_leaving_private");
	const auto project = open_game(dir);
	fs::create_directories(project.snk_dir());
	fs::create_directories(project.cache_dir());
	fs::create_directories(project.code_dir());
	test_util::make_folder_link(project.code_dir() / "snk_link", project.snk_dir());
	test_util::make_folder_link(project.code_dir() / "cache_link", project.cache_dir());
	test_util::make_folder_link(project.code_dir() / "res_link", project.root / "res");

	const prj::leaving_links plain = prj::links_leaving_project(project.code_dir(), project.root);
	const prj::leaving_links named = prj::links_leaving_project(project.code_dir(), project.root, prj::k_link_walk_limit,
		{ project.snk_dir(), project.cache_dir() });

	BOOST_TEST(plain.found.empty(), "inside the project, and no private folder is named");
	std::vector<std::string> names;
	for (const prj::leaving_link& link : named.found) names.push_back(link.link.filename().string());
	std::ranges::sort(names);
	BOOST_TEST((names == std::vector<std::string>{ "cache_link", "snk_link" }));
}

BOOST_AUTO_TEST_CASE(LinksLeavingProject_SaysWhenTheWalkDidNotFinish_AndAMissingFolderHasNone)
{
	test_util::temp_dir dir("prj_code_leaving_limit");
	const auto project = open_game(dir);
	for (int i = 0; i < 6; ++i) {
		dir.write("game/code/f" + std::to_string(i), "x");
	}

	BOOST_TEST(!prj::links_leaving_project(project.code_dir(), project.root, 3).complete);
	BOOST_TEST(prj::links_leaving_project(project.code_dir(), project.root, 100).complete);
	const prj::leaving_links missing = prj::links_leaving_project(project.root / "no_such_folder", project.root);
	BOOST_TEST(missing.complete);
	BOOST_TEST(missing.found.empty());
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

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(LinkBelow_RefusesAnEntryItCannotRead)
{
	if (::geteuid() == 0) {
		BOOST_TEST_MESSAGE("root reads every folder: nothing to refuse");
		return;
	}
	test_util::temp_dir dir("prj_cache_unreadable");
	dir.write("cache/locked/inside.txt", "x");
	const fs::path locked = dir.path() / "cache" / "locked";
	fs::permissions(locked, fs::perms::none);
	struct restore_on_exit {
		fs::path path;
		~restore_on_exit() { std::error_code ec; fs::permissions(path, fs::perms::owner_all, ec); }
	} restore{ locked };

	const auto problem = prj::link_below(dir.path() / "cache");
	BOOST_REQUIRE(problem.has_value());
	BOOST_TEST(problem->find("cannot be checked for links") != std::string::npos);
}
#endif

#ifdef _WIN32
BOOST_AUTO_TEST_CASE(Refuses_AJunctionAsTheCacheOrInsideIt)
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
		const auto problem = inside ? prj::link_below(project.cache_dir()) : prj::verify_project_mounts(project);
		BOOST_REQUIRE(problem.has_value());
		BOOST_TEST(problem->find("is a link") != std::string::npos);
		BOOST_TEST_MESSAGE("junction refused: " << *problem);
	}
}
#endif

BOOST_AUTO_TEST_SUITE_END()
