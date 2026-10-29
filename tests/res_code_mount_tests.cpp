#include <boost/test/unit_test.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/project/prj_code.h>
// Not test_scene_env.hpp: res_vfs_resolver.h brings in Asio and so <windows.h>.
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// code:// over <project>/code (#450): what it sees, what it writes, and what it refuses.
namespace {

namespace fs = std::filesystem;

std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> out(s.size());
	for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
	return out;
}

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

bool wait_for_content(const fs::path& p, std::string_view expected)
{
	for (int i = 0; i < 500; ++i) {
		if (fs::exists(p) && read_file(p) == expected) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

struct code_fixture
{
	test_util::temp_dir dir{ "res_code_mount" };
	fs::path project = dir.path() / "project";
	fs::path content = project / "res";
	fs::path code = project / "code";
	fs::path cache = project / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };

	code_fixture()
	{
		fs::create_directories(content / "levels");
		fs::create_directories(code / "src");
		fs::create_directories(cache / "memory");
		fs::create_directories(project / "build");
		dir.write("project/res/levels/main.desc", "level");
		dir.write("project/code/src/game.cpp", "int main() {}");
		dir.write("project/code/CMakeLists.txt", "project(game)");
		dir.write("project/build/out.cpp", "generated");
	}

	res::mount_table table() const
	{
		return res::resource_system::project_mount_table({}, { content }, cache, cache / "user_profile", code);
	}
};

}

BOOST_AUTO_TEST_SUITE(ResCodeMount)

BOOST_FIXTURE_TEST_CASE(MountTable_HasCodeOnlyWithACodeDir, code_fixture)
{
	const auto with_code = table();
	const auto code_mount = std::ranges::find_if(with_code, [](const res::mount& m) { return m.protocol == res::tag::code; });
	BOOST_REQUIRE(code_mount != with_code.end());
	BOOST_TEST((code_mount->roots == std::vector<fs::path>{ code }));
	BOOST_TEST((code_mount->access == res::mount_access::read_write));

	const auto without = res::resource_system::project_mount_table({}, { content }, cache, cache / "user_profile");
	BOOST_TEST(std::ranges::none_of(without, [](const res::mount& m) { return m.protocol == res::tag::code; }));
}

BOOST_FIXTURE_TEST_CASE(Code_SeesTheSources_ResDoesNot_AndCodeDoesNotSeeAssets, code_fixture)
{
	res::resource_system sys(tasks, table());

	BOOST_TEST(sys.exists(res::tag{ "code://src/game.cpp" }));
	BOOST_TEST(sys.exists(res::tag{ "code://CMakeLists.txt" }));
	BOOST_TEST(!sys.exists(res::tag{ "res://src/game.cpp" }));
	BOOST_TEST(!sys.exists(res::tag{ "code://levels/main.desc" }));
	BOOST_TEST(sys.exists(res::tag{ "res://levels/main.desc" }));
}

BOOST_FIXTURE_TEST_CASE(Code_ReadsAsText, code_fixture)
{
	res::resource_system sys(tasks, table());

	auto handle = sys.require_sync<res::text_resource>(res::tag{ "code://src/game.cpp" });
	test_util::pump(tasks);
	BOOST_REQUIRE(!handle.has_error());
	BOOST_TEST(handle.get_sync()->str() == "int main() {}");
}

BOOST_FIXTURE_TEST_CASE(Code_StoresCodeFilesInsideCode, code_fixture)
{
	res::resource_system sys(tasks, table());

	BOOST_TEST(sys.store(res::tag{ "code://src/player.cpp" }, to_bytes("player")));
	BOOST_TEST(sys.store(res::tag{ "code://include/player.h" }, to_bytes("header")));
	BOOST_TEST(sys.store(res::tag{ "code://CMakeLists.txt" }, to_bytes("project(changed)")));
	BOOST_TEST(wait_for_content(code / "src" / "player.cpp", "player"));
	BOOST_TEST(wait_for_content(code / "include" / "player.h", "header"));
	BOOST_TEST(wait_for_content(code / "CMakeLists.txt", "project(changed)"));
}

BOOST_FIXTURE_TEST_CASE(Code_RefusesFilesThatAreNotCode, code_fixture)
{
	res::resource_system sys(tasks, table());

	BOOST_TEST(!sys.store(res::tag{ "code://notes.md" }, to_bytes("x")));
	BOOST_TEST(!sys.store(res::tag{ "code://src/run.sh" }, to_bytes("x")));
	BOOST_TEST(!sys.store(res::tag{ "code://src/game.cpp.exe" }, to_bytes("x")));
	BOOST_TEST(!fs::exists(code / "notes.md"));
}

BOOST_FIXTURE_TEST_CASE(Code_RefusesIgnoredFoldersInsideCode, code_fixture)
{
	res::resource_system sys(tasks, table());
	fs::create_directories(code / "build");
	fs::create_directories(code / ".git");

	for (const char* tag : { "code://build/out.cpp", "code://Build/out.cpp", "code://.git/hook.cpp", "code://.cache/a.cpp",
	                         "code://src/_deps/lib.cpp", "code://src/build/gen.h" }) {
		BOOST_TEST_INFO(tag);
		BOOST_TEST(!sys.store(res::tag{ tag }, to_bytes("x")));
	}
	BOOST_TEST(!fs::exists(code / "build" / "out.cpp"));
	BOOST_TEST(!fs::exists(code / ".git" / "hook.cpp"));
}

BOOST_FIXTURE_TEST_CASE(Code_RefusesToEscapeTheFolder, code_fixture)
{
	res::resource_system sys(tasks, table());

	BOOST_TEST(!sys.store(res::tag{ "code://../build/out.cpp" }, to_bytes("x")));
	BOOST_TEST(!sys.store(res::tag{ "code://../res/levels/new.cpp" }, to_bytes("x")));
	BOOST_TEST(!sys.store(res::tag{ "code://src/../../.cache/a.cpp" }, to_bytes("x")));
	BOOST_TEST(read_file(project / "build" / "out.cpp") == "generated");
	BOOST_TEST(!fs::exists(content / "levels" / "new.cpp"));
	BOOST_TEST(!fs::exists(cache / "a.cpp"));
}

BOOST_FIXTURE_TEST_CASE(Code_RefusesAWriteThroughALinkedFolder, code_fixture)
{
	fs::create_directories(dir.path() / "outside");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", code / "linked", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	res::resource_system sys(tasks, table());

	BOOST_TEST(!sys.store(res::tag{ "code://linked/escape.cpp" }, to_bytes("x")));
	BOOST_TEST(!fs::exists(dir.path() / "outside" / "escape.cpp"));
}

#ifdef _WIN32
BOOST_FIXTURE_TEST_CASE(Code_RefusesAWriteThroughAJunction, code_fixture)
{
	fs::create_directories(dir.path() / "outside");
	const std::string command = "cmd /c mklink /J \"" + (code / "junction").string() + "\" \"" + (dir.path() / "outside").string() + "\" >nul";
	if (std::system(command.c_str()) != 0 || !fs::exists(code / "junction")) {
		BOOST_TEST_MESSAGE("junctions unavailable");
		return;
	}
	res::resource_system sys(tasks, table());

	BOOST_TEST(!sys.store(res::tag{ "code://junction/escape.cpp" }, to_bytes("x")));
	BOOST_TEST(!fs::exists(dir.path() / "outside" / "escape.cpp"));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
