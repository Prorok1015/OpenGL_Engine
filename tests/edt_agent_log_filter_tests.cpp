#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_agent_log_filter.h"
#include <filesystem>
#include <string>
#include <system_error>

// #935: what log.read gives an agent of the engine's journal.
namespace {

namespace fs = std::filesystem;

// A disk that has /usr, /etc and /home with anything below them, whatever the machine of the test has.
bool on_a_made_up_disk(const fs::path& folder)
{
	const std::string text = folder.generic_string();
	return text.starts_with("/usr/") || text.starts_with("/etc/") || text.starts_with("/home/");
}

edt::agent_log_roots roots()
{
	edt::agent_log_roots out;
	out.project = { "/home/dev/games/demo" };
	out.home = { "/home/dev" };
	out.temp = { "/tmp" };
	out.on_disk = on_a_made_up_disk;
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EdtAgentLogFilterTests)

BOOST_AUTO_TEST_CASE(SomeCategories_AreNeverAnAgents_TheBuildOnlyWhileItMayNotBuild)
{
	for (const char* hidden : { "edt/decoder", "capture", "mcp", "mcp/http", "editor/agent_server", "editor/agent_policy" }) {
		BOOST_TEST_CONTEXT(hidden) {
			BOOST_TEST(!edt::agent_log_category_visible(hidden, false));
			BOOST_TEST(!edt::agent_log_category_visible(hidden, true));
		}
	}
	BOOST_TEST(!edt::agent_log_category_visible("editor/build", false));
	BOOST_TEST(!edt::agent_log_category_visible("editor/build/cmake", false));
	BOOST_TEST(edt::agent_log_category_visible("editor/build", true));
	for (const char* shown : { "desc", "scene", "resource/write", "play", "editor", "editor/builder", "mcpx", "captures" }) {
		BOOST_TEST_CONTEXT(shown) {
			BOOST_TEST(edt::agent_log_category_visible(shown, false));
		}
	}
}

BOOST_AUTO_TEST_CASE(PathsOfTheProjectKeepTheirTail_TheOthersBecomeAnonymous)
{
	const edt::agent_log_roots known = roots();
	BOOST_TEST(edt::agent_log_text("loaded /home/dev/games/demo/res/levels/a.desc", known) == "loaded <project>/res/levels/a.desc");
	BOOST_TEST(edt::agent_log_text("token file '/home/dev/.config/snk/token' missing", known) == "token file '<home>/.config/snk/token' missing");
	BOOST_TEST(edt::agent_log_text("scratch /tmp/snk_decode_17/in.glb", known) == "scratch <tmp>/snk_decode_17/in.glb");
	BOOST_TEST(edt::agent_log_text("cmake at /usr/bin/cmake, config (/etc/snk.conf)", known) == "cmake at <path>, config (<path>)");
	BOOST_TEST(edt::agent_log_text(R"(wrote C:\Users\dev\secret.txt and \\server\share\x)", known) == "wrote <path> and <path>");
	BOOST_TEST(edt::agent_log_text("path=/usr/lib/x.so;next", known) == "path=<path>;next");
}

BOOST_AUTO_TEST_CASE(WhatOnlyLooksLikeAPath_IsLeftAlone)
{
	// Every folder is there: only the rules about what comes before a token keep these.
	edt::agent_log_roots known = roots();
	known.on_disk = [](const fs::path&) { return true; };
	for (const char* text : { "res://levels/a.desc /header/type: unknown texture type", "ratio 3/4, either/or", "https://example.org/x",
		"a.desc /components/mesh/geometry: not found", "<project>/res/a.desc", "memory://import/a.desc: /usr/bin is not a field",
		"'levels/a.desc' (/etc/x/y)", "x // y/z", "see /usr" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, known) == text);
		}
	}
	// A folder of the user is one after a resource too.
	BOOST_TEST(edt::agent_log_text("res://a.desc /home/dev/x", known) == "res://a.desc <home>/x");
}

BOOST_AUTO_TEST_CASE(WithoutRoots_OnlyAbsolutePathsGo)
{
	edt::agent_log_roots none;
	none.on_disk = [](const fs::path& folder) { return folder.generic_string() == "/home/x"; };
	BOOST_TEST(edt::agent_log_text("at /home/x/y and /header/type", none) == "at <path> and /header/type");
	BOOST_TEST(edt::agent_log_text("", none) == "");
}

BOOST_AUTO_TEST_CASE(APathStarts_AfterAnythingAPathCannotHold_AndAfterACompilerFlag)
{
	const edt::agent_log_roots known = roots();
	// The message of a std::filesystem::filesystem_error.
	BOOST_TEST(edt::agent_log_text("filesystem error: cannot copy: No such file or directory [/home/dev/games/demo/a.txt] [/home/dev/b.txt]", known)
		== "filesystem error: cannot copy: No such file or directory [<project>/a.txt] [<home>/b.txt]");
	BOOST_TEST(edt::agent_log_text("{/usr/bin/x}|/etc/y/z|", known) == "{<path>}|<path>|");
	BOOST_TEST(edt::agent_log_text("cc -I/usr/include/x -I/home/dev/games/demo/code @/tmp/x.rsp @/usr/lib/y.rsp", known)
		== "cc -I<path> -I<project>/code @<tmp>/x.rsp @<path>");
	BOOST_TEST(edt::agent_log_text(R"(cl /FoC:\proj\out\a.obj -IC:\sdk\include /IC:\x\y)", known) == "cl /Fo<path> -I<path> /I<path>");
}

BOOST_AUTO_TEST_CASE(FileAddressesAndShares_AreFiles_OtherAddressesAreNot)
{
	const edt::agent_log_roots known = roots();
	BOOST_TEST(edt::agent_log_text("open file:///home/dev/games/demo/a.txt", known) == "open file://<project>/a.txt");
	BOOST_TEST(edt::agent_log_text("open file:///usr/share/x and file:///opt/none/y", known) == "open file://<path> and file://<path>");
	BOOST_TEST(edt::agent_log_text("file:///C:/Users/dev/x.txt", known) == "file:///<path>");
	BOOST_TEST(edt::agent_log_text("file://server/share/x.txt", known) == "file:<path>");
	BOOST_TEST(edt::agent_log_text(R"(copy //server/share/x.txt to \\nas\media\y)", known) == "copy <path> to <path>");
	BOOST_TEST(edt::agent_log_text(R"(\\?\C:\Users\dev\x)", known) == R"(\\?\<path>)");
	BOOST_TEST(edt::agent_log_text("x // y/z and https://example.org/usr/bin", known) == "x // y/z and https://example.org/usr/bin");
}

BOOST_AUTO_TEST_CASE(APathOutsideTheRoots_GoesOnThroughOneSpace_WhenWhatFollowsIsMoreOfIt)
{
	const edt::agent_log_roots known = roots();
	BOOST_TEST(edt::agent_log_text(R"(ran C:\Program Files\CMake\bin\cmake.exe --version)", known) == "ran <path> --version");
	BOOST_TEST(edt::agent_log_text("/usr/local dir/bin/x and more", known) == "<path> and more");
	BOOST_TEST(edt::agent_log_text(R"(C:\a\b  c\d)", known) == R"(<path>  c\d)");
	BOOST_TEST(edt::agent_log_text("/usr/bin/cmake /usr/bin/ninja", known) == "<path> <path>");
	BOOST_TEST(edt::agent_log_text("/usr/bin/x: error in a/b", known) == "<path>: error in a/b");
}

BOOST_AUTO_TEST_CASE(ARoot_MatchesOnlyAWholeFolderName)
{
	edt::agent_log_roots known;
	known.project = { "/p/demo/" };
	known.home = { "/home/u" };
	known.on_disk = [](const fs::path&) { return false; };
	BOOST_TEST(edt::agent_log_text("open /p/demo/a.desc", known) == "open <project>/a.desc");
	BOOST_TEST(edt::agent_log_text("in /p/demo", known) == "in <project>");
	BOOST_TEST(edt::agent_log_text("in /p/demo.", known) == "in <project>.");
	BOOST_TEST(edt::agent_log_text("open /home/u/a", known) == "open <home>/a");
	for (const char* text : { "open /p/demo2/a.desc", "open /p/demo.bak/a.desc", "open /home/u2/a", "open /home/u-old/a" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, known) == text);
		}
	}
}

#ifdef _WIN32
BOOST_AUTO_TEST_CASE(OnWindows_ARootIsFoundInAnyLetterCase)
{
	edt::agent_log_roots known;
	known.project = { R"(C:\Games\Demo)" };
	BOOST_TEST(edt::agent_log_text(R"(open c:\games\demo\res\a.desc and C:/GAMES/DEMO/b.desc)", known) == R"(open <project>\res\a.desc and <project>/b.desc)");
	BOOST_TEST(edt::agent_log_text(R"(open c:\games\demo2\a.desc)", known) == "open <path>");
}
#endif

BOOST_AUTO_TEST_CASE(AJsonPointer_IsNoPath_UnlessItsFirstTwoSegmentsAreOnTheDisk)
{
	const test_util::temp_dir dir{ "edt_agent_log_filter_disk" };
	fs::create_directories(dir.path() / "sub");
	// No roots and the real disk.
	const edt::agent_log_roots none{};
	for (const char* text : { "bad value at /components/0/name", "/header/type: unknown texture type", "see /snk_no_such_folder_935/x/y" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, none) == text);
		}
	}
	BOOST_TEST(edt::agent_log_text("read " + (dir.path() / "sub" / "x.txt").string(), none) == "read <path>");
	BOOST_TEST(edt::agent_log_text("read " + (dir.path() / "sub" / "x.txt").generic_string(), none) == "read <path>");
}

BOOST_AUTO_TEST_CASE(TheRootsOfAProcess_AreAbsoluteWithoutATrailingSeparator)
{
	const test_util::temp_dir dir{ "edt_agent_log_filter_roots" };
	fs::create_directories(dir.path() / "real");
	const edt::agent_log_roots found = edt::agent_log_roots_of(dir.path() / "real" / "");
	BOOST_REQUIRE(!found.project.empty());
	BOOST_TEST((found.project.front() == dir.path() / "real"));
	BOOST_TEST(edt::agent_log_text("open " + (dir.path() / "real" / "a.desc").generic_string(), found) == "open <project>/a.desc");
	BOOST_TEST(edt::agent_log_text("open " + (dir.path() / "real2" / "a.desc").generic_string(), found).find("<project>") == std::string::npos);
	BOOST_TEST(edt::agent_log_roots_of({}).project.empty());
}

#ifndef _WIN32
// Making a link needs a privilege on Windows.
BOOST_AUTO_TEST_CASE(ALinkedProjectFolder_IsARootUnderBothNames)
{
	const test_util::temp_dir dir{ "edt_agent_log_filter_link" };
	fs::create_directories(dir.path() / "real");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "real", dir.path() / "link", ec);
	BOOST_REQUIRE_MESSAGE(!ec, ec.message());
	const edt::agent_log_roots found = edt::agent_log_roots_of(dir.path() / "link");
	BOOST_TEST(found.project.size() == 2u);
	BOOST_TEST(edt::agent_log_text("open " + (dir.path() / "link" / "a.desc").string(), found) == "open <project>/a.desc");
	BOOST_TEST(edt::agent_log_text("open " + (fs::canonical(dir.path() / "real") / "a.desc").string(), found) == "open <project>/a.desc");
}
#endif

BOOST_AUTO_TEST_SUITE_END()
