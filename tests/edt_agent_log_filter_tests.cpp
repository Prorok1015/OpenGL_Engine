#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_agent_log_filter.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// #935: what log.read gives an agent of the engine's journal.
namespace {

namespace fs = std::filesystem;

// A made-up listing: /usr, /etc and /home with anything below them, and two folders with a space in the name.
edt::agent_log_roots roots()
{
	edt::agent_log_roots out;
	out.project = { "/home/dev/games/demo" };
	out.home = { "/home/dev" };
	out.temp = { "/tmp" };
	out.disk = edt::agent_log_disk({ "/usr/local dir", "C:/Program Files" }, { "/usr", "/etc", "/home" });
	return out;
}

edt::agent_log_roots with_names(std::vector<std::string> names, std::vector<std::string> unlisted = {}, bool whole_root = false)
{
	edt::agent_log_roots out;
	out.disk = edt::agent_log_disk(std::move(names), std::move(unlisted), whole_root);
	return out;
}

bool has(const std::vector<std::string>& all, const std::string& one)
{
	return std::find(all.begin(), all.end(), one) != all.end();
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
	// /usr and /etc are known by their first folder only: after a resource name that is not enough.
	const edt::agent_log_roots known = roots();
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

BOOST_AUTO_TEST_CASE(AfterAResourceName_AListedPathIsStillAPath_AJsonPointerIsNot)
{
	const edt::agent_log_roots known = with_names({ "/usr/bin", "/etc/fstab" });
	BOOST_TEST(edt::agent_log_text("res://a.desc /usr/bin/cmake", known) == "res://a.desc <path>");
	BOOST_TEST(edt::agent_log_text("levels/a.desc: /etc/fstab is not a field", known) == "levels/a.desc: <path> is not a field");
	for (const char* text : { "res://a.desc /components/0/name", "a.desc /usr/name: not found", "a.desc /usr/binary/x" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, known) == text);
		}
	}
}

BOOST_AUTO_TEST_CASE(ALineNumberAfterAPath_StaysAfterTheStandIn)
{
	const edt::agent_log_roots known = with_names({ "/etc/fstab", "/usr/include" });
	BOOST_TEST(edt::agent_log_text("bad line /etc/fstab:3", known) == "bad line <path>:3");
	BOOST_TEST(edt::agent_log_text("/usr/include/x.h:10:5: error: y", known) == "<path>:10:5: error: y");
	BOOST_TEST(edt::agent_log_text(R"(C:\src\a.cpp:42 and more)", known) == "<path>:42 and more");
	BOOST_TEST(edt::agent_log_text("see /etc/fstab.", known) == "see <path>.");
}

BOOST_AUTO_TEST_CASE(AFolderWithASpace_IsAPathOnlyWhenTheListingHasIt)
{
	const edt::agent_log_roots listed = with_names({ "/media/John Smith", "C:/Users/John Smith" });
	BOOST_TEST(edt::agent_log_text("open /media/John Smith/x.txt now", listed) == "open <path> now");
	BOOST_TEST(edt::agent_log_text("open /media/John Smith", listed) == "open <path>");
	BOOST_TEST(edt::agent_log_text(R"(open C:\Users\John Smith\x.txt now)", listed) == "open <path> now");
	BOOST_TEST(edt::agent_log_text("open /media/John Smithers/x.txt", listed) == "open /media/John Smithers/x.txt");

	// Only the first folder is known: the space ends the path, and the sentence after it is not swallowed.
	const edt::agent_log_roots unlisted = with_names({}, { "/media" });
	BOOST_TEST(edt::agent_log_text("open /media/John Smith/x.txt", unlisted) == "open <path> Smith/x.txt");
	BOOST_TEST(edt::agent_log_text("read /media/usb and/or the other one", unlisted) == "read <path> and/or the other one");
	BOOST_TEST(edt::agent_log_text(R"(open C:\Users\John Smith\x.txt)", unlisted) == R"(open <path> Smith\x.txt)");
}

BOOST_AUTO_TEST_CASE(APathList_IsRewrittenElementByElement_HoweverLongTheLine)
{
	edt::agent_log_roots known = with_names({ "/usr/bin" }, { "/etc" });
	std::string line = "PATH=";
	std::string expected = "PATH=";
	for (std::size_t element = 0; line.size() < 64 * 1024; ++element) {
		if (element > 0) {
			line += ':';
			expected += ':';
		}
		const char* const texts[3][2] = { { "/usr/bin", "<path>" }, { "/opt/none", "/opt/none" }, { "/etc/alternatives", "<path>" } };
		line += texts[element % 3][0];
		expected += texts[element % 3][1];
	}
	BOOST_TEST((edt::agent_log_text(line, known) == expected));

	// A token longer than the scanner looks at: its start still goes.
	const std::string endless = "/usr/bin/" + std::string(64 * 1024, 'a');
	BOOST_TEST(edt::agent_log_text(endless, known).starts_with("<path>"));
}

// The scanner takes the listing, never the disk: a folder that is really there is no path without it.
BOOST_AUTO_TEST_CASE(ALine_IsOnlyComparedWithTheListing_NeverWithTheDisk)
{
	const test_util::temp_dir dir{ "edt_agent_log_filter_no_disk" };
	fs::create_directories(dir.path() / "sub");
	const std::string real = "/" + (dir.path().relative_path() / "sub" / "x.txt").generic_string();
	BOOST_TEST(edt::agent_log_text("read " + real, edt::agent_log_roots{}) == "read " + real);
	BOOST_TEST(edt::agent_log_text("at /snk_no_such_folder_935/x/y", with_names({ "/snk_no_such_folder_935/x" })) == "at <path>");
}

BOOST_AUTO_TEST_CASE(TheListingOfADisk_HoldsTwoLevels_WithinItsLimits_WithoutTheSkippedFolders)
{
	const test_util::temp_dir dir{ "edt_agent_log_filter_listing" };
	for (const char* file : { "a/one/deep.txt", "a/two.txt", "b/John Smith/x.txt", "skip/hidden.txt", "top.txt",
		"big/0", "big/1", "big/2", "big/3", "big/4" }) {
		fs::create_directories((dir.path() / file).parent_path());
		std::ofstream(dir.path() / file) << "x";
	}
	const std::vector<std::string> skipped{ "skip" };

	const edt::agent_log_disk found = edt::agent_log_disk_of(dir.path(), skipped, edt::agent_log_disk_limits{ 4096, 4 });
	for (const char* name : { "/a/one", "/a/two.txt", "/b/John Smith" }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(has(found.names(), name));
		}
	}
	BOOST_TEST(found.names().size() == 3u);
	BOOST_TEST(std::is_sorted(found.names().begin(), found.names().end()));
	BOOST_TEST(has(found.unlisted(), "/skip"));
	BOOST_TEST(has(found.unlisted(), "/big"));
	BOOST_TEST(found.unlisted().size() == 2u);
	BOOST_TEST(!found.whole_root());

	edt::agent_log_roots known;
	known.disk = found;
	BOOST_TEST(edt::agent_log_text("read /a/one/deep.txt, /b/John Smith/x.txt, /skip/hidden.txt, /big/9 and /a/none /top.txt", known)
		== "read <path>, <path>, <path>, <path> and /a/none /top.txt");

	for (const std::size_t total : { std::size_t{ 0 }, std::size_t{ 1 }, std::size_t{ 3 } }) {
		BOOST_TEST_CONTEXT(total) {
			// A folder met after the total is used up is not entered, and what is below it is still a path.
			edt::agent_log_roots cut;
			cut.disk = edt::agent_log_disk_of(dir.path(), skipped, edt::agent_log_disk_limits{ total, 512 });
			BOOST_TEST(cut.disk.names().size() <= total);
			BOOST_TEST(cut.disk.unlisted().size() <= 5u);
			BOOST_TEST(!cut.disk.whole_root());
			BOOST_TEST(edt::agent_log_text("read /a/one/deep.txt, /skip/hidden.txt and /big/9", cut) == "read <path>, <path> and <path>");
		}
	}
	const edt::agent_log_disk none = edt::agent_log_disk_of(dir.path() / "no_such_folder", {});
	BOOST_TEST(none.names().empty());
	BOOST_TEST(none.unlisted().empty());
	BOOST_TEST(!none.whole_root());
}

// A root with more unread folders than the limit gives up its list: every rooted token is a path then.
BOOST_AUTO_TEST_CASE(ARootWithTooManyUnreadFolders_CountsAsAPathWhole)
{
	const test_util::temp_dir dir{ "edt_agent_log_filter_overfull" };
	for (const char* file : { "a/x", "b/x", "c/x", "d/x" }) {
		fs::create_directories((dir.path() / file).parent_path());
		std::ofstream(dir.path() / file) << "x";
	}
	edt::agent_log_roots known;
	known.disk = edt::agent_log_disk_of(dir.path(), {}, edt::agent_log_disk_limits{ 0, 2 });
	BOOST_TEST(known.disk.whole_root());
	BOOST_TEST(known.disk.names().empty());
	BOOST_TEST(known.disk.unlisted().size() == 2u);
	BOOST_TEST(edt::agent_log_text("read /a/x, /d/x and /nowhere", known) == "read <path>, <path> and <path>");

	known.disk = edt::agent_log_disk_of(dir.path(), {}, edt::agent_log_disk_limits{ 0, 4 });
	BOOST_TEST(!known.disk.whole_root());
	BOOST_TEST(known.disk.unlisted().size() == 4u);
	BOOST_TEST(edt::agent_log_text("read /a/x, /d/x and /nowhere", known) == "read <path>, <path> and /nowhere");
}

// The names are ordered when the listing is made, whatever order they are given in; a lookup goes by the first segment.
BOOST_AUTO_TEST_CASE(AListing_OrdersItsNames_AndFindsEachByItsFirstSegment)
{
	const edt::agent_log_disk disk({ "/usr/local dir", "/a b/c", "/usr/bin", "/a/b", "/ab/c", "/", "/single", "", "/a/b c" }, {});
	BOOST_TEST(disk.names().size() == 6u);
	BOOST_TEST(std::is_sorted(disk.names().begin(), disk.names().end()));
	edt::agent_log_roots known;
	known.disk = disk;
	for (const char* path : { "/usr/local dir/x", "/a b/c/x", "/usr/bin/x", "/a/b/x", "/ab/c/x", "/a/b c/x", "/usr/bin" }) {
		BOOST_TEST_CONTEXT(path) {
			BOOST_TEST(edt::agent_log_text(std::string("open ") + path + " now", known) == "open <path> now");
		}
	}
	for (const char* text : { "at /usr/lib/x", "at /a/c", "at /ab/d", "at /single/x", "at /us/bin", "at /usr", "at /usr/" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, known) == text);
		}
	}

	// A first segment with '\' cannot be ordered beside "/a/...": it counts whole instead.
	const edt::agent_log_disk odd({ "/a\\b/c", "/a/d" }, {});
	BOOST_TEST(odd.names().size() == 1u);
	BOOST_REQUIRE(odd.unlisted().size() == 1u);
	BOOST_TEST(odd.unlisted().front() == "/a\\b");
}

// The root itself is not known (on Windows: the current drive is not a listed one): "/Users/x" is a path whole.
BOOST_AUTO_TEST_CASE(ARootThatIsNotListed_MakesEveryRootedTokenAPath)
{
	const edt::agent_log_roots known = with_names({}, {}, true);
	BOOST_TEST(edt::agent_log_text("open /Users/dev/x.txt:3 now", known) == "open <path>:3 now");
	BOOST_TEST(edt::agent_log_text("open /Users and /a/b.", known) == "open <path> and <path>.");
	BOOST_TEST(edt::agent_log_text("res://a.desc: /header/type is bad", known) == "res://a.desc: /header/type is bad");
	BOOST_TEST(edt::agent_log_text("a / b, and/or, http://host/x, 1/2", known) == "a / b, and/or, http://host/x, 1/2");
	BOOST_TEST(edt::agent_log_text("open /Users/dev/x.txt", edt::agent_log_roots{}) == "open /Users/dev/x.txt");
}

BOOST_AUTO_TEST_CASE(WithoutRoots_OnlyAbsolutePathsGo)
{
	const edt::agent_log_roots none = with_names({ "/home/x" });
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
	BOOST_TEST(edt::agent_log_text("x // y/z and https://example.org/usr/bin", known) == "x // y/z and https://example.org/usr/bin");
}

BOOST_AUTO_TEST_CASE(APathOutsideTheRoots_GoesOnThroughASpace_OnlyInAFolderTheListingHas)
{
	const edt::agent_log_roots known = roots();
	BOOST_TEST(edt::agent_log_text(R"(ran C:\Program Files\CMake\bin\cmake.exe --version)", known) == "ran <path> --version");
	BOOST_TEST(edt::agent_log_text("/usr/local dir/bin/x and more", known) == "<path> and more");
	BOOST_TEST(edt::agent_log_text(R"(C:\a\b  c\d)", known) == R"(<path>  c\d)");
	BOOST_TEST(edt::agent_log_text("/usr/bin/cmake /usr/bin/ninja", known) == "<path> <path>");
	BOOST_TEST(edt::agent_log_text("/usr/bin/x: error in a/b", known) == "<path>: error in a/b");
	// Not in the listing: the next word is not the path's, even when it has a separator.
	BOOST_TEST(edt::agent_log_text("/usr/share dir/bin/x and more", known) == "<path> dir/bin/x and more");
	BOOST_TEST(edt::agent_log_text(R"(ran C:\Other Files\x.exe)", known) == R"(ran <path> Files\x.exe)");
}

BOOST_AUTO_TEST_CASE(ARoot_MatchesOnlyAWholeFolderName)
{
	edt::agent_log_roots known;
	known.project = { "/p/demo/" };
	known.home = { "/home/u" };
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

BOOST_AUTO_TEST_CASE(ALongPathPrefix_GoesWithThePathItStandsBefore)
{
	const edt::agent_log_roots known = roots();
	BOOST_TEST(edt::agent_log_text(R"(open \\?\C:\Users\dev\x.txt now)", known) == "open <path> now");
	BOOST_TEST(edt::agent_log_text(R"(open \\.\C:\Users\dev\x.txt)", known) == "open <path>");
	BOOST_TEST(edt::agent_log_text(R"(open \\?\UNC\nas\media\clips\x.mp4, then stop)", known) == "open <path>, then stop");
	BOOST_TEST(edt::agent_log_text(R"(open \\?\unc\nas\media\x)", known) == "open <path>");
	// Neither a drive nor a share follows: not a path this scanner knows.
	BOOST_TEST(edt::agent_log_text(R"(open \\?\UNC\nas)", known) == R"(open \\?\UNC\nas)");
}

#ifdef _WIN32
BOOST_AUTO_TEST_CASE(OnWindows_ARootIsFoundBehindALongPathPrefix)
{
	edt::agent_log_roots on_drive;
	on_drive.project = { R"(C:\Games\Demo)" };
	BOOST_TEST(edt::agent_log_text(R"(open \\?\C:\Games\Demo\res\a.desc)", on_drive) == R"(open <project>\res\a.desc)");
	BOOST_TEST(edt::agent_log_text(R"(open \\?\C:\Games\Demo2\a.desc)", on_drive) == "open <path>");

	edt::agent_log_roots on_share;
	on_share.project = { R"(\\nas\games\demo)" };
	BOOST_TEST(edt::agent_log_text(R"(open \\nas\games\demo\a.desc)", on_share) == R"(open <project>\a.desc)");
	BOOST_TEST(edt::agent_log_text(R"(open \\?\UNC\nas\games\demo\a.desc)", on_share) == R"(open <project>\a.desc)");
	BOOST_TEST(edt::agent_log_text(R"(open \\?\UNC\NAS\Games\Demo)", on_share) == "open <project>");
	BOOST_TEST(edt::agent_log_text(R"(open \\?\UNC\nas\games\demo2\a.desc)", on_share) == "open <path>");
}

BOOST_AUTO_TEST_CASE(OnWindows_AnMsysPath_IsThePathOfItsDrive)
{
	edt::agent_log_roots known;
	known.project = { R"(C:\Games\Demo)" };
	known.disk = edt::agent_log_disk({ "D:/Users", "C:/Users", "c:/Windows" }, {});
	BOOST_TEST(edt::agent_log_text("open /C/windows/x and /d/users", known) == "open <path> and <path>");
	BOOST_TEST(edt::agent_log_text("open /c/Games/Demo/res/a.desc and /C/games/demo", known) == "open <project>/res/a.desc and <project>");
	BOOST_TEST(edt::agent_log_text("open /c/Games/Demo2/a.desc", known) == "open /c/Games/Demo2/a.desc");
	BOOST_TEST(edt::agent_log_text("open /c/Users/dev/x.txt", known) == "open <path>");
	BOOST_TEST(edt::agent_log_text("bad value at /a/b/name", known) == "bad value at /a/b/name");
}

BOOST_AUTO_TEST_CASE(OnWindows_ADriveThatIsNotListed_IsAPathWhole)
{
	edt::agent_log_roots known;
	known.disk = edt::agent_log_disk({}, { "Z:" });
	BOOST_TEST(edt::agent_log_text(R"(open Z:\share\x now)", known) == "open <path> now");
	BOOST_TEST(edt::agent_log_text("open z:/share/x.txt:3", known) == "open <path>:3");
	BOOST_TEST(edt::agent_log_text("open /z/share/x now", known) == "open <path> now");
	BOOST_TEST(edt::agent_log_text("open /y/share/x and /z", known) == "open /y/share/x and /z");
}

// The current drive is a network one: it is not read, and a path without a drive on it is a path whole, as "/z/x" is.
BOOST_AUTO_TEST_CASE(OnWindows_ACurrentDriveThatIsNotListed_MakesADrivelessPathAPathWhole)
{
	edt::agent_log_roots known;
	known.project = { R"(Z:\Games\Demo)" };
	known.disk = edt::agent_log_disk({ "C:/Users" }, { "Z:" }, true);
	BOOST_TEST(edt::agent_log_text(R"(open /Users/dev/x.txt and \Users\dev)", known) == R"(open <path> and \Users\dev)");
	BOOST_TEST(edt::agent_log_text("open /share/x now, /z/share/x and /c/Users/dev", known) == "open <path> now, <path> and <path>");
	BOOST_TEST(edt::agent_log_text("open /z/Games/Demo/res/a.desc", known) == "open <project>/res/a.desc");
	BOOST_TEST(edt::agent_log_text("res://a.desc: /header/type is bad", known) == "res://a.desc: /header/type is bad");
}
#else
BOOST_AUTO_TEST_CASE(OffWindows_AnMsysPath_IsAnAbsolutePathLikeAnyOther)
{
	edt::agent_log_roots known;
	known.project = { "/c/Games/Demo" };
	known.disk = edt::agent_log_disk({ "/c/Users" }, {});
	BOOST_TEST(edt::agent_log_text("open /c/Games/Demo/res/a.desc", known) == "open <project>/res/a.desc");
	BOOST_TEST(edt::agent_log_text("open /c/Users/dev/x.txt", known) == "open <path>");
	BOOST_TEST(edt::agent_log_text("open /C/games/demo/a and /c/none/x", known) == "open /C/games/demo/a and /c/none/x");
}
#endif

// #935: a job's result is the answer of its command; only its message fields are rewritten, never its data.
BOOST_AUTO_TEST_CASE(AJobResult_HasOnlyItsMessageFieldsRewritten)
{
	cmd::log_read_filter filter;
	edt::install_agent_log_filter(filter, [] { return false; }, roots());
	const boost::json::value result = boost::json::parse(R"json({
		"text": "+open /etc/secret/key",
		"output": "wrote /etc/secret/key",
		"exit_code": 1,
		"error": { "code": "git_failed", "message": "fatal: cannot read /etc/secret/key", "stderr_tail": "at /home/dev/games/demo/a.txt" },
		"report": { "error": "stopped at /etc/secret/key", "files": [
			{ "path": "/etc/secret/key", "notes": ["kept /etc/secret/key", 7], "warnings": [{ "message": "see /etc/secret/key" }] } ] },
		"a": { "b": { "c": { "d": { "message": "too deep /etc/secret/key" } } } }
	})json");

	const boost::json::value as_agent = edt::agent_job_result(result, &filter, cmd::origin::agent("s1"));
	const boost::json::object& shown = as_agent.as_object();
	BOOST_TEST(shown.at("text").as_string().c_str() == std::string("+open /etc/secret/key"));
	BOOST_TEST(shown.at("output").as_string().c_str() == std::string("wrote <path>"));
	BOOST_TEST(shown.at("exit_code").as_int64() == 1);
	BOOST_TEST(shown.at("error").at("code").as_string().c_str() == std::string("git_failed"));
	BOOST_TEST(shown.at("error").at("message").as_string().c_str() == std::string("fatal: cannot read <path>"));
	BOOST_TEST(shown.at("error").at("stderr_tail").as_string().c_str() == std::string("at <project>/a.txt"));
	BOOST_TEST(shown.at("report").at("error").as_string().c_str() == std::string("stopped at <path>"));
	const boost::json::object& file = shown.at("report").at("files").as_array().at(0).as_object();
	BOOST_TEST(file.at("path").as_string().c_str() == std::string("/etc/secret/key"));
	BOOST_TEST(file.at("notes").as_array().at(0).as_string().c_str() == std::string("kept <path>"));
	BOOST_TEST(file.at("notes").as_array().at(1).as_int64() == 7);
	BOOST_TEST(file.at("warnings").as_array().at(0).at("message").as_string().c_str() == std::string("see <path>"));
	BOOST_TEST(shown.at("a").at("b").at("c").at("d").at("message").as_string().c_str() == std::string("too deep /etc/secret/key"));

	BOOST_TEST((edt::agent_job_result(result, &filter, cmd::origin::ui()) == result));
	BOOST_TEST((edt::agent_job_result(result, nullptr, cmd::origin::agent("s1")) == result));
}

// #935: `text` is a message only beside `code` (a migration answer); the {text, truncated} of vcs.diff is data.
BOOST_AUTO_TEST_CASE(AJobResultsText_IsRewrittenOnlyBesideACode_NeverADiff)
{
	cmd::log_read_filter filter;
	edt::install_agent_log_filter(filter, [] { return false; }, roots());
	const boost::json::value migration = boost::json::parse(R"json({
		"code": 2, "status": "refused", "id": "m1", "refused": "locked /etc/secret/key",
		"text": "rollback of m1: cannot open /etc/secret/key"
	})json");
	const boost::json::value diff = boost::json::parse(R"json({ "text": "+open /etc/secret/key", "truncated": false })json");
	const boost::json::value nested = boost::json::parse(R"json({ "code": 0, "inner": { "text": "+open /etc/secret/key" } })json");

	const boost::json::value shown = edt::agent_job_result(migration, &filter, cmd::origin::agent("s1"));
	BOOST_TEST(shown.at("text").as_string().c_str() == std::string("rollback of m1: cannot open <path>"));
	BOOST_TEST(shown.at("refused").as_string().c_str() == std::string("locked <path>"));
	BOOST_TEST(shown.at("code").as_int64() == 2);
	BOOST_TEST((edt::agent_job_result(migration, &filter, cmd::origin::ui()) == migration));

	BOOST_TEST((edt::agent_job_result(diff, &filter, cmd::origin::agent("s1")) == diff));
	BOOST_TEST((edt::agent_job_result(nested, &filter, cmd::origin::agent("s1")) == nested));
}

BOOST_AUTO_TEST_CASE(AJsonPointer_IsNoPath_UnlessTheListingHasItsFirstTwoSegments)
{
	const edt::agent_log_roots known = with_names({ "/etc/fstab", "/components/0" });
	for (const char* text : { "bad value at /components/1/name", "/header/type: unknown texture type", "see /etc/hosts", "see /etc" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(edt::agent_log_text(text, known) == text);
		}
	}
	BOOST_TEST(edt::agent_log_text("bad value at /components/0/name", known) == "bad value at <path>");
}

BOOST_AUTO_TEST_CASE(OnlyAFixedDriveFromC_IsListed_EveryOtherTypeIsNotRead)
{
	// The values of GetDriveTypeW.
	constexpr unsigned unknown = 0, no_root_dir = 1, removable = 2, fixed = 3, remote = 4, cdrom = 5, ramdisk = 6;
	BOOST_TEST(fixed == edt::k_agent_log_drive_fixed);
	BOOST_TEST(edt::agent_log_drive_is_listed('C', fixed));
	BOOST_TEST(edt::agent_log_drive_is_listed('Z', fixed));
	BOOST_TEST(edt::agent_log_drive_is_listed('d', fixed));
	for (const unsigned type : { unknown, no_root_dir, removable, remote, cdrom, ramdisk, 7u, 0xFFFFFFFFu }) {
		BOOST_TEST_CONTEXT(type) {
			BOOST_TEST(!edt::agent_log_drive_is_listed('C', type));
			BOOST_TEST(!edt::agent_log_drive_is_listed('Z', type));
		}
	}
	for (const char letter : { 'A', 'B', 'a', 'b', '1', '/', '\0' }) {
		BOOST_TEST_CONTEXT(static_cast<int>(letter)) {
			BOOST_TEST(!edt::agent_log_drive_is_listed(letter, fixed));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheMountTable_GivesTheFirstLevelNetworkMounts_WithTheirEscapesDecoded)
{
	const std::string mounts =
		"/dev/sda1 / ext4 rw,relatime 0 0\n"
		"/dev/sdb1 /data ext4 rw,relatime 0 0\n"
		"server:/export /nfs3 nfs rw,vers=3 0 0\n"
		"server:/export /nfs4 nfs4 rw,vers=4.2 0 0\n"
		"//server/share /cifs cifs rw 0 0\n"
		"//server/share /smb smb3 rw 0 0\n"
		"/etc/auto.misc /misc autofs rw,relatime 0 0\n"
		"user@host:/srv /sshfs fuse.sshfs rw,nosuid 0 0\n"
		"server:/deep /mnt/nas nfs rw 0 0\n"
		"server:/a\\040b /mnt\\040share nfs4 rw 0 0\n"
		"server:/t /tab\\011bed nfs rw 0 0\n"
		"server:/n /new\\012line cifs rw 0 0\n"
		"server:/s /back\\134slash cifs rw 0 0\n"
		"portal /run fuse.portal rw 0 0\n"
		"host /plan9 9p rw,trans=virtio 0 0\n"
		"mon1:6789:/ /cephfs ceph rw 0 0\n"
		"host:/vol /gluster glusterfs rw 0 0\n"
		"host:/vol /glusterfuse fuse.glusterfs rw 0 0\n"
		"https://dav.example/ /dav davfs rw 0 0\n"
		"remote: /cloud fuse.rclone rw 0 0\n"
		"bare /barefuse fuse rw 0 0\n"
		"/dev/sdc1 /ntfs fuseblk rw 0 0\n"
		"/dev/sdd1 /fused fused rw 0 0\n"
		"tmpfs /tmp tmpfs rw 0 0\n"
		"broken /half\n"
		"\n"
		"server:/last /last nfs";
	const std::vector<std::string> expected{
		"nfs3", "nfs4", "cifs", "smb", "misc", "sshfs", "mnt share", "tab\tbed", "new\nline", "back\\slash", "run",
		"plan9", "cephfs", "gluster", "glusterfuse", "dav", "cloud", "barefuse", "last",
	};
	const std::vector<std::string> found = edt::agent_log_remote_mounts(mounts);
	BOOST_TEST(found == expected, boost::test_tools::per_element());
	BOOST_TEST(edt::agent_log_remote_mounts("").empty());
	BOOST_TEST(edt::agent_log_remote_mounts("server:/x / nfs rw 0 0\n").empty());
	const std::vector<std::string> not_an_escape = edt::agent_log_remote_mounts("server:/x /a\\04 nfs rw 0 0\r\n");
	BOOST_REQUIRE(not_an_escape.size() == 1u);
	BOOST_TEST(not_an_escape.front() == "a\\04");
}

// The listing of the machine itself stays within its limits and holds nothing of the folders it does not enter.
BOOST_AUTO_TEST_CASE(TheListingOfThisMachine_StaysWithinItsLimits)
{
	const edt::agent_log_disk found = edt::agent_log_disk_at_start();
	const edt::agent_log_disk_limits limits{};
	BOOST_TEST(found.names().size() <= limits.total);
	// Each root (the drive-less one and 26 drives) gives per_folder names at most, and a drive may be there itself.
	BOOST_TEST(found.unlisted().size() <= 27u * (limits.per_folder + 2u));
#ifdef __linux__
	for (const std::string& name : found.names()) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(!name.starts_with("/proc/"));
			BOOST_TEST(!name.starts_with("/sys/"));
			BOOST_TEST(!name.starts_with("/dev/"));
		}
	}
#endif
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
