#include <boost/test/unit_test.hpp>
#include "test_file_attributes.hpp"
#include "test_folder_link.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_source_rule.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The one rule of where an agent's import may read from (#1378, #1380): the picture and the model import share it.
// Told from the text and the volume tables only; the path is never looked at. Then (#1408) the walk that refuses a
// link on the way, never following one.

namespace {

namespace fs = std::filesystem;

bool refused(const std::string& why)
{
	return !why.empty();
}

// An absolute path on this system: the walk of #1408 iterates fs::path.
fs::path absolute_path(std::string_view rest)
{
#if defined(_WIN32)
	return fs::path("C:/") / fs::path(rest);
#else
	return fs::path("/") / fs::path(rest);
#endif
}

using platform::beneath::link_entry;

// Ports whose disk is a list of entries: what is not listed is missing; every path asked is recorded.
struct fake_disk
{
	edt::source_volume_ports ports(bool windows = false)
	{
		edt::source_volume_ports out;
		out.windows = windows;
		out.entry_of = [this](const fs::path& path, std::error_code& ec) -> std::optional<link_entry> {
			ec.clear();
			asked.push_back(path);
			order.push_back("open " + path.generic_string());
			for (const auto& [at, kind] : entries) {
				if (at == path) return kind;
			}
			if (path == broken) ec = std::make_error_code(std::errc::permission_denied);
			return std::nullopt;
		};
		out.file_attributes = [this](const fs::path& path) -> std::optional<std::uint32_t> {
			order.push_back("list " + path.generic_string());
			return path == cloud ? cloud_attributes : 0x20u;
		};
		return out;
	}

	std::vector<std::pair<fs::path, link_entry>> entries;
	std::vector<fs::path> asked;
	std::vector<std::string> order;
	fs::path broken;
	fs::path cloud;
	std::uint32_t cloud_attributes = 0;
};

}

BOOST_AUTO_TEST_SUITE(EdtAgentSourceRuleTests)

BOOST_AUTO_TEST_CASE(OnWindows_ASourceIsLocalOnlyOnAFixedOrRemovableDrive)
{
	int asked = 0;
	edt::source_volume_ports windows;
	windows.windows = true;
	unsigned type = edt::k_drive_fixed;
	windows.drive_type = [&](char letter) {
		++asked;
		BOOST_CHECK_EQUAL(letter, 'D');
		return type;
	};
	windows.mount_table = {};
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:\\pictures\\rock.png", windows)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:/models/robot.glb", windows)));
	type = edt::k_drive_removable;
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:\\rock.png", windows)));
	for (const unsigned other : { 0u, 1u, 4u, 5u, 6u }) {
		type = other;
		BOOST_CHECK(refused(edt::agent_source_path_problem("D:\\rock.png", windows)));
	}
	BOOST_CHECK_GT(asked, 0);
	windows.drive_type = {};
	BOOST_CHECK(refused(edt::agent_source_path_problem("D:\\rock.png", windows)));
}

BOOST_AUTO_TEST_CASE(SharesDevicePathsAndDotComponents_AreRefusedFromTheTextAlone)
{
	int asked = 0;
	edt::source_volume_ports windows;
	windows.windows = true;
	windows.drive_type = [&](char) {
		++asked;
		return edt::k_drive_fixed;
	};
	for (const char* path : { "\\\\server\\share\\x.png", "//server/share/x.glb", "\\\\?\\UNC\\server\\share\\x.glb",
		"\\\\?\\D:\\x.png", "\\\\.\\D:\\x.png", "\\??\\D:\\x.png", "/\\server\\x.png", "\\pictures\\x.png",
		"pictures\\x.png", "D:x.png", "D:\\a\\..\\x.png", "D:\\x.png:stream", "" }) {
		BOOST_TEST_CONTEXT(path)
		{
			BOOST_CHECK(refused(edt::agent_source_path_problem(path, windows)));
		}
	}
	BOOST_CHECK_EQUAL(asked, 0);

	int read = 0;
	edt::source_volume_ports posix;
	posix.windows = false;
	posix.drive_type = {};
	posix.mount_table = [&]() -> std::optional<std::string> {
		++read;
		return std::string("/dev/sda1 / ext4 rw 0 0\n");
	};
	for (const char* path : { "\\\\server\\share\\x.glb", "//server/share/x.glb", "\\\\?\\UNC\\server\\share\\x.glb",
		"/home/../x.glb", "x.glb" }) {
		BOOST_TEST_CONTEXT(path)
		{
			const std::string why = edt::agent_source_path_problem(path, posix);
			BOOST_CHECK(refused(why));
		}
	}
	BOOST_CHECK_EQUAL(read, 0);
	const std::string share = edt::agent_source_path_problem("//server/share/x.glb", posix);
	BOOST_CHECK_MESSAGE(share.find("network share") != std::string::npos, share);
}

BOOST_AUTO_TEST_CASE(OnPosix_TheLongestMountPointDecides)
{
	edt::source_volume_ports posix;
	posix.windows = false;
	posix.drive_type = {};
	std::optional<std::string> table = std::string("/dev/sda1 / ext4 rw 0 0\nnas:/x /mnt/nas nfs4 rw 0 0\n"
		"/dev/sdb1 /mnt/nas/local ext4 rw 0 0\nhost:/y /media/net\\040share cifs rw 0 0\n");
	posix.mount_table = [&] { return table; };
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/home/user/rock.png", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/mnt/nas/rock.png", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/mnt//nas/robot.glb", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/mnt/nas", posix)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/mnt/nasty/rock.png", posix)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/mnt/nas/local/robot.glb", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/media/net share/rock.png", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/home/../mnt/nas/rock.png", posix)));
	// A table that cannot be read refuses every path.
	table.reset();
	BOOST_CHECK(refused(edt::agent_source_path_problem("/home/user/rock.png", posix)));
	posix.mount_table = {};
	BOOST_CHECK(refused(edt::agent_source_path_problem("/home/user/rock.png", posix)));
}

BOOST_AUTO_TEST_CASE(OnPosix_OnlyALocalFileSystemTypeIsAllowed)
{
	edt::source_volume_ports posix;
	posix.windows = false;
	posix.drive_type = {};
	const std::string table = "/dev/sda1 / ext4 rw 0 0\nproc /proc proc rw 0 0\nnas:/x /mnt/nfs nfs rw 0 0\n"
		"//host/y /mnt/smb cifs rw 0 0\nme@host:/z /mnt/ssh fuse.sshfs rw 0 0\nodd /mnt/odd someunknownfs rw 0 0\n"
		"tmpfs /mnt/ram tmpfs rw 0 0\noverlay /mnt/layer overlay rw 0 0\n/dev/sdb1 /mnt/data xfs rw 0 0\n"
		"sysfs /mnt/sys sysfs rw 0 0\n";
	posix.mount_table = [&]() -> std::optional<std::string> { return table; };
	for (const char* path : { "/mnt/nfs/x.glb", "/mnt/smb/x.glb", "/mnt/ssh/x.glb", "/mnt/odd/x.glb",
		"/mnt/sys/x.glb" }) {
		BOOST_TEST_CONTEXT(path)
		{
			const std::string why = edt::agent_source_path_problem(path, posix);
			BOOST_CHECK(refused(why));
			BOOST_CHECK_MESSAGE(why.find("no local disk") != std::string::npos, why);
		}
	}
	for (const char* path : { "/home/user/x.glb", "/mnt/ram/x.glb", "/mnt/layer/x.glb", "/mnt/data/x.glb" }) {
		BOOST_TEST_CONTEXT(path)
		{
			BOOST_CHECK(!refused(edt::agent_source_path_problem(path, posix)));
		}
	}
	// /proc/self/root/... reaches any file of the system (a share's too): refused by its text.
	int read = 0;
	posix.mount_table = [&]() -> std::optional<std::string> {
		++read;
		return table;
	};
	for (const char* path : { "/proc/self/root/mnt/nfs/x.glb", "/proc", "/proc/1/cwd/x.glb" }) {
		BOOST_TEST_CONTEXT(path)
		{
			const std::string why = edt::agent_source_path_problem(path, posix);
			BOOST_CHECK_MESSAGE(why.find("/proc") != std::string::npos, why);
		}
	}
	BOOST_CHECK_EQUAL(read, 0);
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/procedures/x.glb", posix)));
}

BOOST_AUTO_TEST_CASE(OnPosix_NestedMounts_TheLongestPrefixDecides)
{
	edt::source_volume_ports posix;
	posix.windows = false;
	posix.drive_type = {};
	posix.mount_table = []() -> std::optional<std::string> {
		return std::string("/dev/sda1 / ext4 rw 0 0\nnas:/x /mnt/nas nfs rw 0 0\n"
			"/dev/sdb1 /mnt/nas/local ext4 rw 0 0\n");
	};
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/home/x.glb", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/mnt/nas/x.glb", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/mnt/nas/localx/x.glb", posix)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/mnt/nas/local/x.glb", posix)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/mnt/nas/local/deep/x.glb", posix)));
}

BOOST_AUTO_TEST_CASE(WithoutPaths_NamesTheRootsAsResAndTheSourceByItsName)
{
	const test_util::temp_dir dir{ "agent_source_rule" };
	const fs::path top = dir.path() / "project" / "res";
	const fs::path source = dir.path() / "outside" / "rock.png";
	const std::string text = top.string() + "/textures/x.png, " + top.generic_string() + " and " + source.string();
	const std::string clean = edt::without_paths(text, { top }, source);
	BOOST_CHECK_MESSAGE(clean.find("unit_test_tmp") == std::string::npos, clean);
	BOOST_CHECK_MESSAGE(clean.find("res://textures/x.png") != std::string::npos, clean);
	BOOST_CHECK_MESSAGE(clean.find("rock.png") != std::string::npos, clean);

	// No source: only the roots.
	const std::string roots_only = edt::without_paths(top.string() + "/a.desc", { top }, {});
	BOOST_CHECK_EQUAL(roots_only, "res://a.desc");
}

BOOST_AUTO_TEST_CASE(WithoutPaths_NamesTheSourcesFolderNeutrally)
{
	const test_util::temp_dir dir{ "agent_source_rule_folder" };
	const fs::path top = dir.path() / "project" / "res";
	const fs::path source = dir.path() / "outside" / "robot.glb";
	const fs::path side = dir.path() / "outside" / "textures" / "skin.png";
	const std::string text = "cannot read " + side.string() + " (" + side.generic_string() + ") beside "
		+ source.string() + " into " + top.generic_string() + "/models";
	const std::string clean = edt::without_paths(text, { top }, source);
	BOOST_CHECK_MESSAGE(clean.find("unit_test_tmp") == std::string::npos, clean);
	BOOST_CHECK_MESSAGE(clean.find(std::string(edt::k_source_folder_text) + "/textures/skin.png") != std::string::npos,
		clean);
	BOOST_CHECK_MESSAGE(clean.find("robot.glb") != std::string::npos, clean);
	BOOST_CHECK_MESSAGE(clean.find("res://models") != std::string::npos, clean);

	// A source at a root keeps the separators of every other path.
	const std::string rooted = edt::without_paths("/a/b and /robot.glb", {}, fs::path("/robot.glb"));
	BOOST_CHECK_EQUAL(rooted, "/a/b and robot.glb");
}

// #1408: Win32 strips a trailing space or dot, so `x ` and `x` could be two entries on the walk and one on the open.
BOOST_AUTO_TEST_CASE(OnWindows_AComponentEndingInASpaceOrADot_IsRefused)
{
	edt::source_volume_ports windows;
	windows.windows = true;
	windows.drive_type = [](char) { return edt::k_drive_fixed; };
	windows.mount_table = {};
	for (const char* path : { "D:\\models\\x \\robot.glb", "D:\\models.\\robot.glb", "D:\\robot.glb.",
		"D:/a /b.glb", "D:\\robot.glb " }) {
		BOOST_TEST_CONTEXT(path)
		{
			const std::string why = edt::agent_source_path_problem(path, windows);
			BOOST_CHECK_MESSAGE(why.find("ends in a space or a dot") != std::string::npos, why);
		}
	}
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:\\a.b\\robot.glb", windows)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:\\ a\\robot.glb", windows)));
}

// #1408: without_paths replaces the source by its text: a link on its way is never resolved.
BOOST_AUTO_TEST_CASE(WithoutPaths_NeverResolvesTheSourceThroughALink)
{
	const test_util::temp_dir dir{ "agent_source_rule_unresolved" };
	dir.write("real/robot.glb", "glb");
	const fs::path base = fs::weakly_canonical(dir.path());
	test_util::make_folder_link(base / "linked", base / "real");
	const fs::path source = base / "linked" / "robot.glb";
	const fs::path target = base / "real" / "robot.glb";
	const std::string clean = edt::without_paths("read " + source.string() + " as " + target.string(), {}, source);
	BOOST_CHECK_MESSAGE(clean.find("read robot.glb as ") != std::string::npos, clean);
	// The target's spelling would be replaced only had the source been resolved through the link.
	BOOST_CHECK_MESSAGE(clean.find(target.string()) != std::string::npos, clean);
}

// #1408: the walk from the root down refuses at the first link and looks at nothing below it.
BOOST_AUTO_TEST_CASE(TheLinksWalk_StopsAtTheFirstLink_AndLooksAtNothingBelowIt)
{
	fake_disk disk;
	disk.entries = { { absolute_path("a"), link_entry::plain }, { absolute_path("a/b"), link_entry::link },
		{ absolute_path("a/b/c"), link_entry::plain }, { absolute_path("a/b/c/d.glb"), link_entry::plain } };
	const std::string why = edt::agent_source_links_problem(absolute_path("a/b/c/d.glb"), disk.ports());
	BOOST_CHECK_MESSAGE(why.find("'b' on the way to the source is a link") != std::string::npos, why);
	BOOST_REQUIRE_EQUAL(disk.asked.size(), 2u);
	BOOST_CHECK(disk.asked[0] == absolute_path("a"));
	BOOST_CHECK(disk.asked[1] == absolute_path("a/b"));

	disk.entries[1].second = link_entry::plain;
	disk.asked.clear();
	BOOST_CHECK(!refused(edt::agent_source_links_problem(absolute_path("a/b/c/d.glb"), disk.ports())));
	BOOST_CHECK_EQUAL(disk.asked.size(), 4u);
}

BOOST_AUTO_TEST_CASE(TheLinksWalk_EndsAtAMissingEntry_AndRefusesOneThatCannotBeInspected)
{
	fake_disk disk;
	disk.entries = { { absolute_path("a"), link_entry::plain } };
	BOOST_CHECK(!refused(edt::agent_source_links_problem(absolute_path("a/gone/x.glb"), disk.ports())));
	BOOST_CHECK_EQUAL(disk.asked.size(), 2u);
	disk.broken = absolute_path("a/locked");
	const std::string why = edt::agent_source_links_problem(absolute_path("a/locked/x.glb"), disk.ports());
	BOOST_CHECK_MESSAGE(why.find("cannot be inspected") != std::string::npos, why);
	edt::source_volume_ports none;
	none.entry_of = {};
	BOOST_CHECK(refused(edt::agent_source_links_problem(absolute_path("a/x.glb"), none)));
	BOOST_CHECK(refused(edt::agent_source_links_problem(fs::path("a/x.glb"), disk.ports())));
}

BOOST_AUTO_TEST_CASE(TheLinksWalk_BelowAFolderThatWasChecked_LooksOnlyBelowIt)
{
	fake_disk disk;
	disk.entries = { { absolute_path("m"), link_entry::link }, { absolute_path("m/tex"), link_entry::link } };
	const std::string why = edt::agent_source_links_problem(absolute_path("m/tex/skin.png"), disk.ports(),
		absolute_path("m"));
	BOOST_CHECK_MESSAGE(why.find("'tex' on the way to the source is a link") != std::string::npos, why);
	BOOST_REQUIRE_EQUAL(disk.asked.size(), 1u);
	BOOST_CHECK(disk.asked[0] == absolute_path("m/tex"));
	// A name outside that folder is walked whole.
	disk.asked.clear();
	BOOST_CHECK(refused(edt::agent_source_links_problem(absolute_path("m/x.png"), disk.ports(), absolute_path("n"))));
	BOOST_CHECK(disk.asked.front() == absolute_path("m"));
}

BOOST_AUTO_TEST_CASE(ACloudFile_IsRefusedOnlyWhenOpeningItWouldFetchItFromTheNetwork)
{
	BOOST_CHECK(!refused(edt::cloud_attributes_problem(0x20u)));
	// A placeholder kept on this disk (a reparse point, pinned) is a local file.
	BOOST_CHECK(!refused(edt::cloud_attributes_problem(0x400u | 0x20u | 0x80000u)));
	for (const std::uint32_t fetched : { edt::k_file_attribute_recall_on_data_access,
		edt::k_file_attribute_recall_on_open, edt::k_file_attribute_offline }) {
		const std::string why = edt::cloud_attributes_problem(0x400u | fetched);
		BOOST_CHECK_MESSAGE(why.find("make the file available offline") != std::string::npos, why);
	}

	fake_disk disk;
	disk.entries = { { absolute_path("od"), link_entry::kept_in_place },
		{ absolute_path("od/robot.glb"), link_entry::kept_in_place } };
	disk.cloud = absolute_path("od/robot.glb");
	disk.cloud_attributes = 0x400u | edt::k_file_attribute_recall_on_data_access;
	const std::string why = edt::agent_source_links_problem(absolute_path("od/robot.glb"), disk.ports(true));
	BOOST_CHECK_MESSAGE(why.find("available offline") != std::string::npos, why);
	disk.cloud_attributes = 0x400u | 0x20u;
	BOOST_CHECK(!refused(edt::agent_source_links_problem(absolute_path("od/robot.glb"), disk.ports(true))));
}

BOOST_AUTO_TEST_CASE(ARealFolderLink_OnTheWayToTheSource_IsRefused_EvenWhenItsTargetIsGone)
{
	const test_util::temp_dir dir{ "agent_source_links" };
	dir.write("real/robot.glb", "glb");
	const fs::path base = fs::weakly_canonical(dir.path());
	fs::create_directories(base / "gone");
	test_util::make_folder_link(base / "linked", base / "gone");
	fs::remove(base / "gone");
	edt::source_volume_ports ports;
	BOOST_CHECK(!refused(edt::agent_source_links_problem(base / "real" / "robot.glb", ports)));
	const std::string why = edt::agent_source_links_problem(base / "linked" / "robot.glb", ports);
	BOOST_CHECK_MESSAGE(why.find("'linked' on the way to the source is a link") != std::string::npos, why);
}

// #1412 (a): `./tex.png` is common in glTF and a lone `.` cannot pass a link: it is no component; `..` stays refused.
BOOST_AUTO_TEST_CASE(ALoneDot_IsNoComponent_ADotDotIsRefused)
{
	edt::source_volume_ports windows;
	windows.windows = true;
	windows.drive_type = [](char) { return edt::k_drive_fixed; };
	windows.mount_table = {};
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:\\.\\x.png", windows)));
	BOOST_CHECK(!refused(edt::agent_source_path_problem("D:/models/./x.png", windows)));
	const std::string up = edt::agent_source_path_problem("D:\\a\\..\\x.png", windows);
	BOOST_CHECK_MESSAGE(up.find("'..'") != std::string::npos, up);

	edt::source_volume_ports posix;
	posix.windows = false;
	posix.drive_type = {};
	posix.mount_table = [] {
		return std::optional<std::string>("/dev/sda1 / ext4 rw 0 0\nnas:/x /mnt/nas nfs4 rw 0 0\n");
	};
	BOOST_CHECK(!refused(edt::agent_source_path_problem("/home/./x.glb", posix)));
	// The dot does not hide the mount it is under.
	BOOST_CHECK(refused(edt::agent_source_path_problem("/mnt/./nas/x.glb", posix)));
	BOOST_CHECK(refused(edt::agent_source_path_problem("/home/../x.glb", posix)));

	fake_disk disk;
	disk.entries = { { absolute_path("a"), link_entry::plain }, { absolute_path("a/x.glb"), link_entry::plain } };
	BOOST_CHECK(!refused(edt::agent_source_links_problem(absolute_path("a/./x.glb"), disk.ports())));
	BOOST_CHECK_EQUAL(disk.asked.size(), 2u);
	const std::string walked_up = edt::agent_source_links_problem(absolute_path("a/../x.glb"), disk.ports());
	BOOST_CHECK_MESSAGE(walked_up.find("'..'") != std::string::npos, walked_up);
}

// #1412: an entry of a projected file system (VFS for Git) asks its provider when it is opened, so its attributes come
// from its folder's listing first, and an entry a read would fetch is never opened.
BOOST_AUTO_TEST_CASE(OnWindows_TheAttributesComeFromTheListing_BeforeTheEntryIsOpened)
{
	fake_disk disk;
	disk.entries = { { absolute_path("vfs"), link_entry::plain },
		{ absolute_path("vfs/robot.glb"), link_entry::plain } };
	disk.cloud = absolute_path("vfs/robot.glb");
	disk.cloud_attributes = edt::k_file_attribute_recall_on_open;
	const std::string why = edt::agent_source_links_problem(absolute_path("vfs/robot.glb"), disk.ports(true));
	BOOST_CHECK_MESSAGE(why.find("available offline") != std::string::npos, why);
	BOOST_CHECK(std::ranges::find(disk.asked, absolute_path("vfs/robot.glb")) == disk.asked.end());
	BOOST_REQUIRE_GE(disk.order.size(), 3u);
	BOOST_CHECK_EQUAL(disk.order[0], "list " + absolute_path("vfs").generic_string());
	BOOST_CHECK_EQUAL(disk.order[1], "open " + absolute_path("vfs").generic_string());
	BOOST_CHECK_EQUAL(disk.order[2], "list " + absolute_path("vfs/robot.glb").generic_string());
}

// #1412 (review): the long spelling of a path is asked for only once the path as written passed the walk: it lists
// each folder by its path, which would go through a link.
BOOST_AUTO_TEST_CASE(TheLongSpelling_IsAskedForOnlyAfterTheWalkPassed)
{
	fake_disk disk;
	disk.entries = { { absolute_path("a"), link_entry::plain }, { absolute_path("a/b"), link_entry::link },
		{ absolute_path("a/b/x.glb"), link_entry::plain } };
	edt::source_volume_ports ports = disk.ports();
	std::vector<fs::path> spelled;
	ports.long_name = [&](const fs::path& path) {
		spelled.push_back(path);
		return path;
	};
	std::string why;
	BOOST_CHECK(edt::agent_source_spelled(absolute_path("a/b/x.glb"), ports, why).empty());
	BOOST_CHECK_MESSAGE(why.find("'b' on the way to the source is a link") != std::string::npos, why);
	BOOST_CHECK(spelled.empty());

	// A plain path is spelled long and walked again by that spelling.
	disk.entries[1].second = link_entry::plain;
	disk.asked.clear();
	ports = disk.ports();
	ports.long_name = [&](const fs::path& path) {
		spelled.push_back(path);
		return absolute_path("a/b/x.glb");
	};
	const fs::path got = edt::agent_source_spelled(absolute_path("a/B/x.glb"), ports, why);
	BOOST_CHECK_MESSAGE(why.empty(), why);
	BOOST_CHECK(got == absolute_path("a/b/x.glb"));
	BOOST_REQUIRE_EQUAL(spelled.size(), 1u);
	BOOST_CHECK(spelled.front() == absolute_path("a/B/x.glb"));
}

#if defined(_WIN32)
// #1412 (review): a junction met under its 8.3 spelling is refused by the walk, and nothing beyond it is looked at.
BOOST_AUTO_TEST_CASE(OnWindows_AJunctionUnderItsShortName_IsRefused_AndNothingBeyondItIsTouched)
{
	const test_util::temp_dir dir{ "agent_short_junction" };
	dir.write("target/robot.glb", "glTF");
	const fs::path base = fs::weakly_canonical(dir.path());
	test_util::make_folder_link(base / "a_long_junction_name", base / "target");
	const fs::path short_link = test_util::short_path(base / "a_long_junction_name");
	BOOST_REQUIRE_MESSAGE(!short_link.empty() && short_link.filename() != "a_long_junction_name",
		"no 8.3 name for " << (base / "a_long_junction_name").string());
	edt::source_volume_ports ports;
	std::vector<fs::path> asked;
	ports.entry_of = [&](const fs::path& path, std::error_code& ec) {
		asked.push_back(path);
		return platform::beneath::link_entry_at(path, ec);
	};
	bool spelled = false;
	ports.long_name = [&](const fs::path& path) {
		spelled = true;
		return path;
	};
	std::string why;
	BOOST_CHECK(edt::agent_source_spelled(short_link / "robot.glb", ports, why).empty());
	BOOST_CHECK_MESSAGE(why.find("is a link") != std::string::npos, why);
	BOOST_CHECK(!spelled);
	BOOST_REQUIRE(!asked.empty());
	BOOST_CHECK(asked.back() == short_link);
}
#endif

// #1412: a read starts at "/" (POSIX) and names every mount point on its way, so no folder of the path is opened by
// its path; on Windows at the drive.
BOOST_AUTO_TEST_CASE(TheReadRoute_StartsAtTheTop_AndNamesTheMountPointsOnTheWay)
{
	const std::optional<std::string> table = std::string("/dev/sda1 / ext4 rw 0 0\n/dev/sdb1 /mnt/data ext4 rw 0 0\n"
		"/dev/sdc1 /mnt/data/usb vfat rw 0 0\n");
	const auto route = edt::agent_read_route_of(fs::path("/mnt/data/usb/models/robot.glb"), false, table);
	BOOST_REQUIRE(route.has_value());
	BOOST_CHECK(route->root == fs::path("/"));
	BOOST_CHECK_EQUAL(route->folder.generic_string(), "mnt/data/usb/models");
	BOOST_CHECK(route->leaf == fs::path("robot.glb"));
	BOOST_CHECK((route->mount_points == std::vector<std::size_t>{ 1, 2 }));
	const auto dotted = edt::agent_read_route_of(fs::path("/mnt/./database/robot.glb"), false, table);
	BOOST_REQUIRE(dotted.has_value());
	BOOST_CHECK_EQUAL(dotted->folder.generic_string(), "mnt/database");
	BOOST_CHECK(dotted->mount_points.empty());
	BOOST_CHECK(!edt::agent_read_route_of(fs::path("/home/robot.glb"), false, std::nullopt));
	BOOST_CHECK(!edt::agent_read_route_of(fs::path("robot.glb"), false, table));
	BOOST_CHECK(!edt::agent_read_route_of(fs::path("/home/../robot.glb"), false, table));
#if defined(_WIN32)
	const auto drive = edt::agent_read_route_of(fs::path("D:\\models\\robot.glb"), true, std::nullopt);
	BOOST_REQUIRE(drive.has_value());
	BOOST_CHECK(drive->root == fs::path("D:\\"));
	BOOST_CHECK(drive->folder == fs::path("models"));
	BOOST_CHECK(!edt::agent_read_route_of(fs::path("models\\robot.glb"), true, std::nullopt));
#endif
}

namespace {

std::string text_of(const std::vector<std::byte>& bytes)
{
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// read_agent_file's read with `hook` called at its stages.
edt::source_volume_ports hooked(const std::function<void(platform::beneath::read_stage)>& hook)
{
	edt::source_volume_ports ports;
	ports.read_below = [&hook](const platform::beneath::read_request& request,
		const platform::beneath::read_limits& limits, std::vector<std::byte>& out) {
		BOOST_CHECK(limits.same_device);
		BOOST_CHECK(limits.refuse_recall);
		platform::beneath::read_request with_hook = request;
		with_hook.hook = &hook;
		return platform::beneath::read_beneath(with_hook, limits, out);
	};
	return ports;
}

}

// #1412: the bytes come from the handle the walk opened, with its stamp; nothing is opened again by the path.
BOOST_AUTO_TEST_CASE(AnAgentsRead_ComesFromTheHandleThatWasChecked)
{
	const test_util::temp_dir dir{ "agent_read_file" };
	dir.write("real/robot.glb", "glTF-bytes");
	dir.write("real/other.glb", "other");
	const fs::path base = fs::weakly_canonical(dir.path());
	const fs::path file = base / "real" / "robot.glb";
	const edt::agent_file_read read = edt::read_agent_file(file, 64, edt::source_volume_ports{});
	BOOST_REQUIRE_MESSAGE(read.problem.empty(), read.problem);
	BOOST_CHECK_EQUAL(text_of(read.bytes), "glTF-bytes");
	BOOST_REQUIRE(read.stamp);
	const edt::agent_file_read stamped = edt::read_agent_file(file, 64, edt::source_volume_ports{}, true);
	BOOST_REQUIRE_MESSAGE(stamped.problem.empty(), stamped.problem);
	BOOST_CHECK(stamped.bytes.empty());
	BOOST_CHECK(edt::same_file_stamp(*read.stamp, *stamped.stamp));
	const edt::agent_file_read other = edt::read_agent_file(base / "real" / "other.glb", 64, edt::source_volume_ports{});
	BOOST_REQUIRE_MESSAGE(other.problem.empty(), other.problem);
	BOOST_CHECK(!edt::same_file_stamp(*read.stamp, *other.stamp));

	const edt::agent_file_read grown = edt::read_agent_file(file, 4, edt::source_volume_ports{});
	BOOST_CHECK_MESSAGE(grown.problem.find("larger") != std::string::npos, grown.problem);
	BOOST_CHECK(grown.bytes.empty());
	const edt::agent_file_read gone = edt::read_agent_file(base / "real" / "none.glb", 64, edt::source_volume_ports{});
	BOOST_CHECK(!gone.problem.empty());
	BOOST_CHECK(gone.missing);
}

// #1412: a folder swapped for a link after the walk checked it is not followed by the read.
BOOST_AUTO_TEST_CASE(AFolderSwappedForALink_AfterTheCheck_IsNotFollowed)
{
	const test_util::temp_dir dir{ "agent_read_swap" };
	dir.write("real/robot.glb", "real");
	dir.write("other/robot.glb", "other");
	const fs::path base = fs::weakly_canonical(dir.path());
	const fs::path file = base / "real" / "robot.glb";
	BOOST_REQUIRE(!refused(edt::agent_source_links_problem(file, edt::source_volume_ports{})));
	bool swapped = false;
	const std::function<void(platform::beneath::read_stage)> swap = [&](platform::beneath::read_stage stage) {
		if (stage != platform::beneath::read_stage::before_walk) return;
		fs::rename(base / "real", base / "moved");
		swapped = test_util::link_folder(base / "other", base / "real");
	};
	const edt::agent_file_read read = edt::read_agent_file(file, 64, hooked(swap));
	BOOST_REQUIRE(swapped);
	BOOST_CHECK_MESSAGE(!read.problem.empty(), text_of(read.bytes));
	BOOST_CHECK(read.bytes.empty());
	// An agent reads the reason, never the machine's paths.
	BOOST_CHECK_MESSAGE(read.problem.find(base.filename().string()) == std::string::npos, read.problem);
}

#if !defined(_WIN32)
// #1412: the file swapped for a link once its folders are held is not followed either.
BOOST_AUTO_TEST_CASE(AFileSwappedForALink_OnceItsFoldersAreHeld_IsNotFollowed)
{
	const test_util::temp_dir dir{ "agent_read_file_swap" };
	dir.write("real/robot.glb", "real");
	dir.write("other.glb", "other");
	const fs::path base = fs::weakly_canonical(dir.path());
	const std::function<void(platform::beneath::read_stage)> swap = [&](platform::beneath::read_stage stage) {
		if (stage != platform::beneath::read_stage::folders_held) return;
		fs::remove(base / "real" / "robot.glb");
		fs::create_symlink(base / "other.glb", base / "real" / "robot.glb");
	};
	const edt::agent_file_read read = edt::read_agent_file(base / "real" / "robot.glb", 64, hooked(swap));
	BOOST_CHECK_MESSAGE(read.problem.find("no plain file") != std::string::npos, read.problem);
	BOOST_CHECK(read.bytes.empty());
}
#endif

#if defined(_WIN32)
// #1412: an offline file is refused by the walk (from its folder's listing) and by the read (on its handle).
BOOST_AUTO_TEST_CASE(OnWindows_AnOfflineFile_IsRefusedByTheWalkAndByTheRead)
{
	const test_util::temp_dir dir{ "agent_read_offline" };
	dir.write("od/robot.glb", "glTF");
	const fs::path base = fs::weakly_canonical(dir.path());
	const fs::path file = base / "od" / "robot.glb";
	BOOST_REQUIRE(test_util::set_offline(file, true));
	const std::string walked = edt::agent_source_links_problem(file, edt::source_volume_ports{});
	BOOST_CHECK_MESSAGE(walked.find("available offline") != std::string::npos, walked);
	const edt::agent_file_read read = edt::read_agent_file(file, 64, edt::source_volume_ports{});
	BOOST_CHECK_MESSAGE(read.problem.find("available offline") != std::string::npos, read.problem);
	BOOST_CHECK(read.bytes.empty());
	BOOST_REQUIRE(test_util::set_offline(file, false));
	BOOST_CHECK(edt::read_agent_file(file, 64, edt::source_volume_ports{}).problem.empty());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
