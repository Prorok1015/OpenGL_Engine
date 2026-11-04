#include <boost/test/unit_test.hpp>
#include <snk/project/prj_json_text.h>
#include "edt_build_settings.h"
#include "test_temp_dir.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <snk/platform/plt_beneath.h>
#include <snk/platform/plt_reparse_tag.h>

// Files under <project>/.snk are written without following a link below the project root (#523).

namespace {

namespace fs = std::filesystem;

std::string read_text(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// `link` becomes a link to the folder `target`: a symlink on POSIX, a junction on Windows (no privilege needed).
void make_folder_link(const fs::path& target, const fs::path& link)
{
#ifdef _WIN32
	const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
	BOOST_REQUIRE_MESSAGE(std::system(command.c_str()) == 0 && fs::exists(link), "mklink /J failed for " + link.string());
#else
	fs::create_directory_symlink(target, link);
#endif
}

// `link` becomes a symlink to the file `target`; false (with the reason in the test log) when the system refuses it:
// Windows without developer mode or the symlink privilege.
bool make_file_link(const fs::path& target, const fs::path& link)
{
	std::error_code ec;
	fs::create_symlink(target, link, ec);
#ifdef _WIN32
	if (ec) {
		BOOST_TEST_MESSAGE("file symlink not created (" + ec.message() + "): no developer mode or symlink privilege; that case is not run");
		return false;
	}
#endif
	BOOST_REQUIRE_MESSAGE(!ec, "cannot create a symlink: " + ec.message());
	return true;
}

constexpr std::string_view k_victim = R"({"victim":true})";

// What prj::write_project_file does once its look at the file passed, with a hook that changes the tree while the write
// runs: the links planted then are past every check by path.
std::string write_build_json(const fs::path& project_root, std::string_view text, const platform::beneath::write_hook& hook)
{
	const fs::path root = fs::canonical(project_root);
	platform::beneath::pinned_root pinned{ root };
	platform::beneath::write_request request;
	request.folder = ".snk";
	request.leaf = "build.json";
	request.tmp_leaf = ".build.json.race.tmp";
	request.data = std::as_bytes(std::span<const char>(text.data(), text.size()));
	request.hook = &hook;
	request.tmp_path = root / ".snk" / request.tmp_leaf;
	return platform::beneath::write_beneath(pinned, request);
}

}

BOOST_AUTO_TEST_SUITE(PrjWriteProjectFileTests)

BOOST_AUTO_TEST_CASE(Write_CreatesTheFolderAndReplacesTheFile)
{
	test_util::temp_dir dir("prj_write_project_file_plain");
	BOOST_REQUIRE(!prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "one").has_value());
	BOOST_TEST(fs::is_directory(fs::symlink_status(dir.path() / ".snk")));
	BOOST_TEST(read_text(dir.path() / ".snk" / "build.json") == "one");

	BOOST_REQUIRE(!prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "two").has_value());
	BOOST_TEST(read_text(dir.path() / ".snk" / "build.json") == "two");
	const auto entries = std::distance(fs::directory_iterator(dir.path() / ".snk"), fs::directory_iterator{});
	BOOST_TEST(entries == 1); // no temp file left
}

BOOST_AUTO_TEST_CASE(Write_RefusesAPathThatLeavesTheRoot)
{
	test_util::temp_dir dir("prj_write_project_file_escape");
	BOOST_TEST(prj::write_project_file(dir.path(), fs::path("..") / "escape.json", "x").has_value());
	BOOST_TEST(!fs::exists(dir.path().parent_path() / "escape.json"));
}

BOOST_AUTO_TEST_CASE(Write_RefusesALinkAtTheFile_AndLeavesItsTargetAlone)
{
	test_util::temp_dir dir("prj_write_project_file_file_link");
	const fs::path victim = dir.write("victim.json", k_victim);
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(victim, dir.path() / ".snk" / "build.json")) return;

	BOOST_TEST(prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "written").has_value());
	BOOST_TEST(read_text(victim) == k_victim);
	BOOST_TEST(fs::is_symlink(fs::symlink_status(dir.path() / ".snk" / "build.json")));
}

BOOST_AUTO_TEST_CASE(Write_RefusesALinkAtTheFolder_AndWritesNothingIntoItsTarget)
{
	test_util::temp_dir dir("prj_write_project_file_folder_link");
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	make_folder_link(outside, dir.path() / ".snk");

	BOOST_TEST(prj::write_project_file(dir.path(), fs::path(".snk") / "build.json", "written").has_value());
	BOOST_TEST(fs::is_empty(outside));
}

BOOST_AUTO_TEST_CASE(UpdateBuildSettings_RefusesALinkAtBuildJson)
{
	test_util::temp_dir dir("prj_write_project_file_settings_file_link");
	const fs::path victim = dir.write("victim.json", k_victim);
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(victim, dir.path() / ".snk" / "build.json")) return;

	const auto problem = edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	});
	BOOST_TEST(problem.has_value());
	BOOST_TEST(read_text(victim) == k_victim);
}

BOOST_AUTO_TEST_CASE(UpdateBuildSettings_RefusesALinkAtSnk)
{
	test_util::temp_dir dir("prj_write_project_file_settings_folder_link");
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	make_folder_link(outside, dir.path() / ".snk");

	const auto problem = edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	});
	BOOST_TEST(problem.has_value());
	BOOST_TEST(fs::is_empty(outside));
}

BOOST_AUTO_TEST_CASE(UpdateBuildSettings_WritesThroughTheRealWriter)
{
	test_util::temp_dir dir("prj_write_project_file_settings_plain");
	BOOST_REQUIRE(!edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	}).has_value());
	BOOST_TEST(edt::load_build_settings(dir.path() / ".snk").live_enabled);
}

// .snk swapped for a link to another folder once the write holds it: POSIX writes into the folder it holds (now
// .snk_moved), Windows holds it without delete sharing so the swap itself fails; nothing lands in the link's target.
BOOST_AUTO_TEST_CASE(Race_SnkSwappedForALinkWhileHeld_WritesNothingIntoItsTarget)
{
	test_util::temp_dir dir("prj_write_project_file_race_folder");
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	fs::create_directories(dir.path() / ".snk");
	bool swapped = false;
	const platform::beneath::write_hook hook = [&](platform::beneath::write_stage stage, const fs::path&) {
		if (stage != platform::beneath::write_stage::folders_held) return;
		std::error_code ec;
		fs::rename(dir.path() / ".snk", dir.path() / ".snk_moved", ec);
		if (ec) {
			BOOST_TEST_MESSAGE("the held .snk cannot be renamed (" + ec.message() + "): the swap is refused itself");
			return;
		}
		make_folder_link(outside, dir.path() / ".snk");
		swapped = true;
	};
	const std::string error = write_build_json(dir.path(), "raced", hook);
	BOOST_TEST(fs::is_empty(outside));
	if (swapped) {
		BOOST_TEST(error.empty());
		BOOST_TEST(read_text(dir.path() / ".snk_moved" / "build.json") == "raced");
	}
}

// build.json swapped for a link just before the rename: the link itself is replaced, its target never written.
BOOST_AUTO_TEST_CASE(Race_FileSwappedForALinkBeforeTheRename_ReplacesTheLinkNotItsTarget)
{
	test_util::temp_dir dir("prj_write_project_file_race_file");
	const fs::path victim = dir.write("victim.json", k_victim);
	const fs::path file = dir.write(".snk/build.json", "old");
	const platform::beneath::write_hook hook = [&](platform::beneath::write_stage stage, const fs::path&) {
		if (stage != platform::beneath::write_stage::before_rename) return;
		std::error_code ec;
		fs::remove(file, ec);
		BOOST_REQUIRE_MESSAGE(!ec, "cannot remove build.json: " + ec.message());
		make_file_link(victim, file);
	};
	BOOST_TEST(write_build_json(dir.path(), "raced", hook).empty());
	BOOST_TEST(read_text(victim) == k_victim);
	BOOST_TEST(!fs::is_symlink(fs::symlink_status(file)));
	BOOST_TEST(read_text(file) == "raced");
}

// The Windows walk (reads and writes alike, #523): a folder a filter keeps in place is entered through the filter, a
// link or a junction is refused. The decision is pure, so it runs everywhere.
BOOST_AUTO_TEST_CASE(FolderEntry_FilterPlaceholdersAreEntered_LinksAndJunctionsRefused)
{
	using platform::beneath::folder_entry;
	using platform::beneath::folder_entry_of;
	constexpr std::uint32_t directory = platform::beneath::k_file_attribute_directory;
	constexpr std::uint32_t reparse = directory | platform::beneath::k_file_attribute_reparse_point;
	BOOST_TEST((folder_entry_of(directory, true, 0) == folder_entry::enter));
	BOOST_TEST((folder_entry_of(reparse, true, 0x9000001Au) == folder_entry::through_filter)); // IO_REPARSE_TAG_CLOUD (OneDrive)
	BOOST_TEST((folder_entry_of(reparse, true, 0x9000601Au) == folder_entry::through_filter)); // IO_REPARSE_TAG_CLOUD_6
	BOOST_TEST((folder_entry_of(reparse, true, 0x9000F01Au) == folder_entry::through_filter)); // IO_REPARSE_TAG_CLOUD_F
	BOOST_TEST((folder_entry_of(reparse, true, 0x80000013u) == folder_entry::through_filter)); // IO_REPARSE_TAG_DEDUP
	BOOST_TEST((folder_entry_of(reparse, true, 0x80000017u) == folder_entry::refused)); // IO_REPARSE_TAG_WOF: not allowlisted
	BOOST_TEST((folder_entry_of(reparse, true, 0x90001018u) == folder_entry::refused)); // IO_REPARSE_TAG_WCI_1: not allowlisted
	BOOST_TEST((folder_entry_of(reparse, true, 0x9000101Bu) == folder_entry::refused)); // next to CLOUD_1, not a cloud tag
	BOOST_TEST((folder_entry_of(reparse, true, 0xA0000003u) == folder_entry::refused)); // junction
	BOOST_TEST((folder_entry_of(reparse, true, 0xA000000Cu) == folder_entry::refused)); // symlink
	BOOST_TEST((folder_entry_of(reparse, true, 0xA000001Du) == folder_entry::refused)); // LX symlink
	BOOST_TEST((folder_entry_of(reparse, false, 0) == folder_entry::refused)); // tag unreadable
	BOOST_TEST((folder_entry_of(platform::beneath::k_file_attribute_reparse_point, true, 0x9000001Au) == folder_entry::not_a_folder));
	BOOST_TEST((folder_entry_of(0, true, 0) == folder_entry::not_a_folder));
}

BOOST_AUTO_TEST_SUITE_END()
