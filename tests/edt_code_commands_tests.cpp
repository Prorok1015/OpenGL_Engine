#include <boost/test/unit_test.hpp>
#include "edt_code_commands.h"
#include "edt_mcp_bridge_host.h"
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
// Not test_scene_env.hpp: res_vfs_resolver.h brings in Asio and so <windows.h>.
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// code.list/read/write/create/delete/rename (A7, #451): code:// only, versions, UTF-8, CMake judged apart.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using cmd::agent_category;

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::vector<std::byte> bytes_of(std::string_view s)
{
	const auto* first = reinterpret_cast<const std::byte*>(s.data());
	return { first, first + s.size() };
}

struct code_fixture
{
	test_util::temp_dir dir{ "edt_code_commands" };
	fs::path project = dir.path() / "project";
	fs::path content = project / "res";
	fs::path code = project / "code";
	fs::path cache = project / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> sys;
	edt::editor_events events;
	std::vector<edt::agent_action_info> actions;
	edt::connection seen;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;

	code_fixture()
	{
		fs::create_directories(content / "levels");
		fs::create_directories(code / "src");
		fs::create_directories(cache / "memory");
		dir.write("project/res/levels/main.desc", "level");
		dir.write("project/code/src/game.cpp", "int game;\r\nint more;\n");
		dir.write("project/code/src/notes.md", "notes");
		dir.write("project/code/CMakeLists.txt", "project(x)\n");
		dir.write("project/code/build/gen.cpp", "generated");
		dir.write("project/code/.hidden/x.cpp", "hidden");
		sys = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table({}, { content }, cache, cache / "user_profile", code));
		seen = events.agent_action.subscribe([this](const edt::agent_action_info& info) { actions.push_back(info); });
		registrations = edt::register_code_commands(commands, { *sys, &events });
	}

	cmd::result call(const std::string& id, json::value args, cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, std::move(args), std::move(from));
	}

	std::string version_of(const std::string& path)
	{
		const auto read = call("code.read", json::object{ { "path", path } });
		BOOST_REQUIRE_MESSAGE(read.ok, read.error);
		return std::string(read.value.at("version").as_string());
	}

	agent_category category_of(const std::string& id, json::value args)
	{
		cmd::call request;
		request.args = std::move(args);
		return cmd::category_of_call(*commands.find(id), request);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtCodeCommands, code_fixture)

BOOST_AUTO_TEST_CASE(List_ShowsCodeOnly_NeitherOtherFilesNorBuildOrHiddenFolders)
{
	const auto listed = call("code.list", json::object{ { "recursive", true } });
	BOOST_REQUIRE_MESSAGE(listed.ok, listed.error);
	std::vector<std::string> paths;
	for (const auto& entry : listed.value.at("entries").as_array()) paths.emplace_back(entry.at("path").as_string());
	const auto has = [&](std::string_view p) { return std::find(paths.begin(), paths.end(), p) != paths.end(); };
	BOOST_TEST(has("src"));
	BOOST_TEST(has("src/game.cpp"));
	BOOST_TEST(has("CMakeLists.txt"));
	BOOST_TEST(!has("src/notes.md"));
	BOOST_TEST(!has("build"));
	BOOST_TEST(!has("build/gen.cpp"));
	BOOST_TEST(!has(".hidden"));
	BOOST_TEST(!listed.value.at("truncated").as_bool());
}

BOOST_AUTO_TEST_CASE(List_RefusesAFolderOutsideCodeOrIgnored)
{
	for (const char* folder : { "../res", "/etc", "build", ".hidden", "src/../..", "src\\x", "nowhere" }) {
		BOOST_TEST_CONTEXT(folder) {
			BOOST_TEST(call("code.list", json::object{ { "dir", folder } }).code == cmd::codes::failed);
		}
	}
}

BOOST_AUTO_TEST_CASE(Read_GivesLinesWithTheirEndingsAndTheVersion)
{
	const auto whole = call("code.read", json::object{ { "path", "src/game.cpp" } });
	BOOST_REQUIRE_MESSAGE(whole.ok, whole.error);
	BOOST_TEST(whole.value.at("text").as_string() == "int game;\r\nint more;\n");
	BOOST_TEST(whole.value.at("total_lines").as_uint64() == 2u);
	BOOST_TEST(whole.value.at("version").as_string() == edt::code_version_of(bytes_of("int game;\r\nint more;\n")));

	const auto second = call("code.read", json::object{ { "path", "src/game.cpp" }, { "from_line", 2 }, { "line_count", 5 } });
	BOOST_REQUIRE(second.ok);
	BOOST_TEST(second.value.at("text").as_string() == "int more;\n");
	BOOST_TEST(second.value.at("line_count").as_uint64() == 1u);
}

BOOST_AUTO_TEST_CASE(Read_OfAFileThatIsNotUtf8_IsRefused)
{
	dir.write("project/code/src/bin.cpp", "int x = \xff\xfe;");
	const auto read = call("code.read", json::object{ { "path", "src/bin.cpp" } });
	BOOST_TEST(read.code == cmd::codes::failed);
	BOOST_TEST(read.error.find("not a text file") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(PathsOutsideCode_AreRefused_AndNothingOutsideIsTouched)
{
	for (const char* path : { "../res/levels/main.desc", "/abs.cpp", "src\\game.cpp", "build/gen.cpp", ".hidden/x.cpp",
			 "src/notes.md", "src/./game.cpp", "src/game.cpp:stream", "src/game.cpp." }) {
		BOOST_TEST_CONTEXT(path) {
			BOOST_TEST(call("code.read", json::object{ { "path", path } }).code == cmd::codes::failed);
			BOOST_TEST(call("code.write", json::object{ { "path", path }, { "version", "x" }, { "text", "" } }).code == cmd::codes::failed);
			BOOST_TEST(call("code.create", json::object{ { "path", path } }).code == cmd::codes::failed);
		}
	}
	BOOST_TEST(read_file(content / "levels" / "main.desc") == "level");
	BOOST_TEST(read_file(code / "build" / "gen.cpp") == "generated");
}

BOOST_AUTO_TEST_CASE(Write_WithTheVersionRead_ReplacesTheFile_AndTellsTheStatusBar)
{
	const std::string version = version_of("src/game.cpp");
	const auto written = call("code.write", json::object{ { "path", "src/game.cpp" }, { "version", version }, { "text", "int y;\r\n" } });
	BOOST_REQUIRE_MESSAGE(written.ok, written.error);
	BOOST_TEST(read_file(code / "src" / "game.cpp") == "int y;\r\n");
	BOOST_TEST(written.value.at("version").as_string() == edt::code_version_of(bytes_of("int y;\r\n")));
	BOOST_REQUIRE(actions.size() == 1u);
	BOOST_TEST(actions[0].command == "code.write");
	BOOST_TEST(actions[0].target == "src/game.cpp");
	BOOST_TEST(actions[0].bytes == 8u);
	BOOST_TEST(actions[0].session == "s1");
}

BOOST_AUTO_TEST_CASE(Write_WithAnOldVersion_IsStale_AndTheFileStaysAsThePersonLeftIt)
{
	const std::string version = version_of("src/game.cpp");
	dir.write("project/code/src/game.cpp", "int persons_edit;\n");

	const auto written = call("code.write", json::object{ { "path", "src/game.cpp" }, { "version", version }, { "text", "int agent;\n" } });
	BOOST_TEST(written.code == cmd::codes::stale);
	BOOST_TEST(written.value.at("version").as_string() == edt::code_version_of(bytes_of("int persons_edit;\n")));
	BOOST_TEST(read_file(code / "src" / "game.cpp") == "int persons_edit;\n");
	BOOST_TEST(actions.empty());
}

BOOST_AUTO_TEST_CASE(Replace_MatchesByteForByte_ExactlyOnce_AndKeepsCrlf)
{
	dir.write("project/code/src/r.cpp", "x = 1;\r\ny = 1;\r\n");
	const auto twice = call("code.write", json::object{ { "path", "src/r.cpp" }, { "version", version_of("src/r.cpp") },
		{ "replace", json::object{ { "old", "= 1;" }, { "new", "= 2;" } } } });
	BOOST_TEST(twice.code == cmd::codes::failed);
	const auto missing = call("code.write", json::object{ { "path", "src/r.cpp" }, { "version", version_of("src/r.cpp") },
		{ "replace", json::object{ { "old", "= 1;\ny" }, { "new", "" } } } });
	BOOST_TEST(missing.code == cmd::codes::failed);
	BOOST_TEST(read_file(code / "src" / "r.cpp") == "x = 1;\r\ny = 1;\r\n");

	const auto once = call("code.write", json::object{ { "path", "src/r.cpp" }, { "version", version_of("src/r.cpp") },
		{ "replace", json::object{ { "old", "y = 1;" }, { "new", "y = 2;\r\nz = 3;" } } } });
	BOOST_REQUIRE_MESSAGE(once.ok, once.error);
	BOOST_TEST(read_file(code / "src" / "r.cpp") == "x = 1;\r\ny = 2;\r\nz = 3;\r\n");
}

BOOST_AUTO_TEST_CASE(Write_TakesEitherTextOrReplace_AndOnlyAnExistingFile)
{
	const std::string version = version_of("src/game.cpp");
	BOOST_TEST(call("code.write", json::object{ { "path", "src/game.cpp" }, { "version", version } }).code == cmd::codes::invalid_args);
	BOOST_TEST(call("code.write", json::object{ { "path", "src/game.cpp" }, { "version", version }, { "text", "" },
		{ "replace", json::object{ { "old", "int" }, { "new", "long" } } } }).code == cmd::codes::invalid_args);
	BOOST_TEST(call("code.write", json::object{ { "path", "src/new.cpp" }, { "version", version }, { "text", "" } }).code == cmd::codes::failed);
	BOOST_TEST(!fs::exists(code / "src" / "new.cpp"));
}

BOOST_AUTO_TEST_CASE(Write_OverTheLimit_IsRefused)
{
	const std::string big(edt::k_code_file_limit + 1, 'x');
	const auto written = call("code.write", json::object{ { "path", "src/game.cpp" }, { "version", version_of("src/game.cpp") }, { "text", big } });
	BOOST_TEST(written.code == cmd::codes::failed);
	BOOST_TEST(read_file(code / "src" / "game.cpp") == "int game;\r\nint more;\n");
}

BOOST_AUTO_TEST_CASE(Create_NeverOverwrites_AndMakesTheFolders)
{
	const auto made = call("code.create", json::object{ { "path", "src/sub/new.h" }, { "text", "#pragma once\n" } });
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	BOOST_TEST(read_file(code / "src" / "sub" / "new.h") == "#pragma once\n");
	BOOST_TEST(call("code.create", json::object{ { "path", "src/sub/new.h" }, { "text", "again" } }).code == cmd::codes::failed);
	BOOST_TEST(read_file(code / "src" / "sub" / "new.h") == "#pragma once\n");
	BOOST_TEST(call("code.create", json::object{ { "path", "src/readme.md" } }).code == cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(Delete_NeedsTheVersionRead)
{
	BOOST_TEST(call("code.delete", json::object{ { "path", "src/game.cpp" }, { "version", "0000000000000000-0" } }).code == cmd::codes::stale);
	BOOST_TEST(fs::exists(code / "src" / "game.cpp"));

	const auto gone = call("code.delete", json::object{ { "path", "src/game.cpp" }, { "version", version_of("src/game.cpp") } });
	BOOST_REQUIRE_MESSAGE(gone.ok, gone.error);
	BOOST_TEST(!fs::exists(code / "src" / "game.cpp"));
	BOOST_REQUIRE(!actions.empty());
	BOOST_TEST(actions.back().command == "code.delete");
}

BOOST_AUTO_TEST_CASE(Rename_MovesBetweenFolders_NeverOverAFile)
{
	const auto moved = call("code.rename", json::object{ { "from", "src/game.cpp" }, { "to", "lib/game.cpp" } });
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	BOOST_TEST(fs::exists(code / "lib" / "game.cpp"));
	BOOST_TEST(!fs::exists(code / "src" / "game.cpp"));

	dir.write("project/code/src/other.cpp", "other");
	BOOST_TEST(call("code.rename", json::object{ { "from", "src/other.cpp" }, { "to", "lib/game.cpp" } }).code == cmd::codes::failed);
	BOOST_TEST(read_file(code / "lib" / "game.cpp") == "int game;\r\nint more;\n");
	BOOST_TEST(call("code.rename", json::object{ { "from", "src/other.cpp" }, { "to", "../res/x.cpp" } }).code == cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(ACMakeFile_IsJudgedAsCmakePresets_WhateverTheCase)
{
	BOOST_TEST((category_of("code.write", json::object{ { "path", "src/a.cpp" }, { "version", "v" }, { "text", "" } }) == agent_category::code_edit));
	BOOST_TEST((category_of("code.write", json::object{ { "path", "cmakelists.TXT" }, { "version", "v" }, { "text", "" } }) == agent_category::cmake_presets));
	BOOST_TEST((category_of("code.write", json::object{ { "path", "sub/foo.CMake" }, { "version", "v" }, { "text", "" } }) == agent_category::cmake_presets));
	BOOST_TEST((category_of("code.create", json::object{ { "path", "CMakeUserPresets.json" } }) == agent_category::cmake_presets));
	BOOST_TEST((category_of("code.create", json::object{ { "path", "src/a.cpp" } }) == agent_category::code_files));
	BOOST_TEST((category_of("code.delete", json::object{ { "path", "CMakePresets.json" }, { "version", "v" } }) == agent_category::cmake_presets));
	BOOST_TEST((category_of("code.rename", json::object{ { "from", "src/a.cmake" }, { "to", "src/a.cpp" } }) == agent_category::cmake_presets));
	BOOST_TEST((category_of("code.rename", json::object{ { "from", "src/a.cpp" }, { "to", "CMakeLists.txt" } }) == agent_category::cmake_presets));
	BOOST_TEST((category_of("code.rename", json::object{ { "from", "src/a.cpp" }, { "to", "lib/a.cpp" } }) == agent_category::code_files));
	BOOST_TEST((category_of("code.read", json::object{ { "path", "CMakeLists.txt" } }) == agent_category::read));
}

BOOST_AUTO_TEST_CASE(TheReads_AreOfferedToAgents_TheChangesWaitForThePolicy)
{
	BOOST_TEST(edt::agent_may_use(*commands.find("code.list")));
	BOOST_TEST(edt::agent_may_use(*commands.find("code.read")));
	for (const char* id : { "code.write", "code.create", "code.delete", "code.rename" }) {
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST(commands.find(id)->mutates);
		}
	}
}

BOOST_AUTO_TEST_CASE(Utf8_IsStrict)
{
	BOOST_TEST(edt::is_valid_utf8("plain"));
	BOOST_TEST(edt::is_valid_utf8("\xD0\x9F\xD1\x80\xD0\xB8"));
	BOOST_TEST(edt::is_valid_utf8("\xF0\x9F\x98\x80"));
	BOOST_TEST(!edt::is_valid_utf8("\xC0\xAF"));         // overlong '/'
	BOOST_TEST(!edt::is_valid_utf8("\xED\xA0\x80"));     // a surrogate
	BOOST_TEST(!edt::is_valid_utf8("\xF4\x90\x80\x80")); // past U+10FFFF
	BOOST_TEST(!edt::is_valid_utf8("\xE2\x82"));         // cut short
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(EdtCodeCommands_WithoutACodeFolder_SayNoProject)
{
	test_util::temp_dir dir{ "edt_code_commands_none" };
	const fs::path content = dir.path() / "res";
	const fs::path cache = dir.path() / ".cache";
	fs::create_directories(content);
	fs::create_directories(cache / "memory");
	ds::task_system tasks{ test_util::manual_tasks() };
	res::resource_system sys{ tasks, res::resource_system::project_mount_table({}, { content }, cache, cache / "user_profile") };
	cmd::command_registry commands;
	const auto registrations = edt::register_code_commands(commands, { sys, nullptr });
	BOOST_TEST(commands.invoke("code.list", json::object{}, cmd::origin::agent("s1")).code == "no_project");
	BOOST_TEST(commands.invoke("code.read", json::object{ { "path", "a.cpp" } }, cmd::origin::agent("s1")).code == "no_project");
}
