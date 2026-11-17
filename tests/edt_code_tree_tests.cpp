#include <boost/test/unit_test.hpp>
#include "edt_code_tree.h"
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// The Code tree's model and the file operations behind its menu (#450).
namespace {

namespace fs = std::filesystem;

std::vector<std::string> relatives(const std::vector<edt::code_entry>& entries)
{
	std::vector<std::string> out;
	for (const auto& entry : entries) out.push_back(entry.relative + (entry.is_directory ? "/" : ""));
	return out;
}

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct code_tree_fixture
{
	test_util::temp_dir dir{ "edt_code_tree" };
	fs::path project = dir.path() / "project";
	fs::path code = project / "code";
	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> sys;
	std::vector<fs::path> opened;
	std::string open_error;
	std::unique_ptr<edt::code_actions> actions;

	code_tree_fixture()
	{
		fs::create_directories(project / "res");
		fs::create_directories(project / ".cache" / "memory");
		fs::create_directories(code / "src" / "game");
		fs::create_directories(code / "build");
		fs::create_directories(code / ".git");
		fs::create_directories(code / "empty");
		dir.write("project/code/CMakeLists.txt", "project(game)");
		dir.write("project/code/main.cpp", "int main() {}");
		dir.write("project/code/Alpha.h", "#pragma once");
		dir.write("project/code/readme.md", "docs");
		dir.write("project/code/src/game/player.cpp", "player");
		dir.write("project/code/build/gen.cpp", "generated");
		dir.write("project/code/.git/hook.cpp", "hook");
		sys = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table({}, { project / "res" }, project / ".cache", project / ".cache" / "user_profile", code));
		actions = std::make_unique<edt::code_actions>(*sys, edt::code_tree_model{ code },
			[this](const fs::path& file) { opened.push_back(file); return open_error; });
	}
};

}

BOOST_AUTO_TEST_SUITE(EdtCodeTree)

BOOST_FIXTURE_TEST_CASE(List_ShowsCodeFilesAndFolders_FoldersFirst_NoIgnoredOnes, code_tree_fixture)
{
	const edt::code_tree_model model{ code };

	const std::vector<std::string> expected{ "empty/", "src/", "Alpha.h", "CMakeLists.txt", "main.cpp" };
	BOOST_TEST(relatives(model.list("")) == expected, boost::test_tools::per_element());
}

BOOST_FIXTURE_TEST_CASE(List_OfASubfolder_UsesPathsUnderCode, code_tree_fixture)
{
	const edt::code_tree_model model{ code };

	const std::vector<std::string> expected{ "src/game/" };
	BOOST_TEST(relatives(model.list("src")) == expected, boost::test_tools::per_element());
	const std::vector<std::string> inner{ "src/game/player.cpp" };
	BOOST_TEST(relatives(model.list("src/game")) == inner, boost::test_tools::per_element());
}

BOOST_FIXTURE_TEST_CASE(List_MarksTheCategory, code_tree_fixture)
{
	const auto entries = edt::code_tree_model{ code }.list("");
	for (const auto& entry : entries) {
		if (entry.name == "CMakeLists.txt") BOOST_TEST((entry.category == prj::code_category::cmake));
		if (entry.name == "main.cpp") BOOST_TEST((entry.category == prj::code_category::source));
		if (entry.name == "Alpha.h") BOOST_TEST((entry.category == prj::code_category::header));
	}
}

BOOST_FIXTURE_TEST_CASE(List_SkipsLinks, code_tree_fixture)
{
	fs::create_directories(dir.path() / "outside");
	dir.write("outside/stolen.cpp", "x");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", code / "linked", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	for (const auto& entry : edt::code_tree_model{ code }.list("")) {
		BOOST_TEST(entry.name != "linked");
	}
}

BOOST_FIXTURE_TEST_CASE(List_OfAMissingFolder_IsEmpty, code_tree_fixture)
{
	BOOST_TEST(edt::code_tree_model{ code }.list("nope").empty());
}

BOOST_FIXTURE_TEST_CASE(List_HandlesNamesOutsideTheCodePage, code_tree_fixture)
{
	{
		std::ofstream out(code / fs::path{ std::u8string{ u8"\u65e5\u672c.cpp" } }, std::ios::binary);
		out << "int nihon;";
	}

	const auto entries = edt::code_tree_model{ code }.list("");
	const auto found = std::ranges::find_if(entries, [](const edt::code_entry& e) { return e.name == "\xE6\x97\xA5\xE6\x9C\xAC.cpp"; });
	BOOST_REQUIRE(found != entries.end());
	BOOST_TEST(found->relative == "\xE6\x97\xA5\xE6\x9C\xAC.cpp");
	BOOST_TEST(fs::exists(edt::code_tree_model{ code }.physical_path(found->relative)));
}

BOOST_AUTO_TEST_CASE(CheckPath_AcceptsCodeFilesInSubfolders)
{
	BOOST_TEST(edt::check_code_file_path("player.cpp").empty());
	BOOST_TEST(edt::check_code_file_path("src/game/player.hpp").empty());
	BOOST_TEST(edt::check_code_file_path("CMakeLists.txt").empty());
}

BOOST_AUTO_TEST_CASE(CheckPath_RefusesWhatCodeCannotBe)
{
	for (const char* bad : { "", "/abs.cpp", "../x.cpp", "src/../x.cpp", "src//x.cpp", "./x.cpp", "readme.md", "x.cpp ", "x.cpp.",
	                         "a\\b.cpp", "c:x.cpp", "bad*.cpp", "con.cpp", "NUL.h", "build/x.cpp", "src/.hidden/x.cpp", "src/_deps/x.cpp",
	                         "src/", "src/CMakeFiles/x.cpp" }) {
		BOOST_TEST_INFO(bad);
		BOOST_TEST(!edt::check_code_file_path(bad).empty());
	}
}

BOOST_FIXTURE_TEST_CASE(Actions_CreateFile_MakesAnEmptyFileAndFolders, code_tree_fixture)
{
	BOOST_TEST(actions->create_file("src", "enemy.cpp").ok);
	BOOST_TEST(actions->create_file("", "ai/brain/mind.h").ok);

	BOOST_TEST(fs::exists(code / "src" / "enemy.cpp"));
	BOOST_TEST(fs::file_size(code / "src" / "enemy.cpp") == 0u);
	BOOST_TEST(fs::exists(code / "ai" / "brain" / "mind.h"));
}

BOOST_FIXTURE_TEST_CASE(Actions_CreateFile_NeverOverwrites, code_tree_fixture)
{
	const auto result = actions->create_file("", "main.cpp");

	BOOST_TEST(!result.ok);
	BOOST_TEST(result.error.find("already exists") != std::string::npos);
	BOOST_TEST(read_file(code / "main.cpp") == "int main() {}");
}

BOOST_FIXTURE_TEST_CASE(Actions_CreateFile_RefusesBadNames, code_tree_fixture)
{
	BOOST_TEST(!actions->create_file("", "notes.md").ok);
	BOOST_TEST(!actions->create_file("", "../escape.cpp").ok);
	BOOST_TEST(!actions->create_file("build", "gen2.cpp").ok);
	BOOST_TEST(!fs::exists(code / "notes.md"));
	BOOST_TEST(!fs::exists(code / "build" / "gen2.cpp"));
}

BOOST_FIXTURE_TEST_CASE(Actions_RenameFile_KeepsTheFolder, code_tree_fixture)
{
	BOOST_TEST(actions->rename_file("src/game/player.cpp", "hero.cpp").ok);

	BOOST_TEST(!fs::exists(code / "src" / "game" / "player.cpp"));
	BOOST_TEST(read_file(code / "src" / "game" / "hero.cpp") == "player");
}

BOOST_FIXTURE_TEST_CASE(Actions_RenameFile_RefusesAPathAndATakenName, code_tree_fixture)
{
	BOOST_TEST(!actions->rename_file("main.cpp", "src/main.cpp").ok);
	BOOST_TEST(!actions->rename_file("main.cpp", "Alpha.h").ok);
	BOOST_TEST(!actions->rename_file("main.cpp", "main.txt").ok);
	BOOST_TEST(read_file(code / "main.cpp") == "int main() {}");
	BOOST_TEST(read_file(code / "Alpha.h") == "#pragma once");
}

BOOST_FIXTURE_TEST_CASE(Actions_DeleteFile_RemovesIt_AndNothingElse, code_tree_fixture)
{
	BOOST_TEST(actions->delete_file("main.cpp").ok);

	BOOST_TEST(!fs::exists(code / "main.cpp"));
	BOOST_TEST(fs::exists(code / "Alpha.h"));
	BOOST_TEST(!actions->delete_file("readme.md").ok);
	BOOST_TEST(fs::exists(code / "readme.md"));
}

BOOST_FIXTURE_TEST_CASE(Actions_OpenInIde_PassesThePhysicalPath, code_tree_fixture)
{
	BOOST_TEST(actions->open_in_ide("src/game/player.cpp").ok);

	BOOST_REQUIRE(opened.size() == 1u);
	BOOST_TEST((opened[0] == code / "src" / "game" / "player.cpp"));
}

BOOST_FIXTURE_TEST_CASE(Actions_OpenInIde_ReportsWhyItCouldNot, code_tree_fixture)
{
	open_error = "no program is associated";

	const auto result = actions->open_in_ide("main.cpp");

	BOOST_TEST(!result.ok);
	BOOST_TEST(result.error == "no program is associated");
}

BOOST_AUTO_TEST_SUITE_END()
