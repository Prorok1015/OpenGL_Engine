#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_agent_source_rule.h"
#include <filesystem>
#include <optional>
#include <string>

// The one rule of where an agent's import may read from (#1378, #1380): the picture and the model import share it.
// Told from the text and the volume tables only; the path is never looked at.

namespace {

namespace fs = std::filesystem;

bool refused(const std::string& why)
{
	return !why.empty();
}

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
		"pictures\\x.png", "D:x.png", "D:\\a\\..\\x.png", "D:\\.\\x.png", "D:\\x.png:stream", "" }) {
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
		"/home/../x.glb", "/home/./x.glb", "x.glb" }) {
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

BOOST_AUTO_TEST_SUITE_END()
