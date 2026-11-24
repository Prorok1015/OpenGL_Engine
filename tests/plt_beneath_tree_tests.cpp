#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_beneath.h>
#include <snk/platform/plt_beneath_tree_hook.h>
#include <snk/platform/plt_reparse_tag.h>
#include <snk/platform/plt_windows_names.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// #1294: folders made and trees removed below a root without going through a link.
namespace {

namespace fs = std::filesystem;

// A link to a folder at `link`: a directory symlink, or on Windows a junction when `junction`. False when this machine
// cannot make a symlink (Windows without the privilege); the test then has nothing to check. A junction needs no
// privilege: one that cannot be made fails the test instead of skipping it.
bool link_folder(const fs::path& target, const fs::path& link, bool junction = false)
{
#ifdef _WIN32
	if (junction) {
		const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
		const bool made = std::system(command.c_str()) == 0 && fs::exists(link);
		BOOST_CHECK_MESSAGE(made, "mklink /J could not make a junction at " << link.string());
		return made;
	}
#else
	(void)junction;
#endif
	std::error_code ec;
	fs::create_directory_symlink(target, link, ec);
	return !ec;
}

// The kinds of link a test runs with: a symlink, and on Windows a junction as well.
std::vector<bool> link_kinds()
{
#ifdef _WIN32
	return { false, true };
#else
	return { false };
#endif
}

struct tree_fixture {
	test_util::temp_dir dir{ "beneath_tree" };
	fs::path root = fs::canonical(dir.path());
	platform::beneath::pinned_root pinned{ root };

	tree_fixture()
	{
		dir.write("outside/keep.txt", "keep");
	}

	fs::path outside() const { return root / "outside"; }
};

#ifdef _WIN32
// READONLY set on `path` itself, a reparse point included.
bool set_read_only(const fs::path& path)
{
	const HANDLE entry = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr);
	if (entry == INVALID_HANDLE_VALUE) {
		return false;
	}
	FILE_BASIC_INFO basic{};
	bool set = GetFileInformationByHandleEx(entry, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
	basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
	set = set && SetFileInformationByHandle(entry, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
	CloseHandle(entry);
	return set;
}
#endif

// A name the system call cuts at its NUL: it would open "..".
fs::path dot_dot_nul()
{
	return fs::path{ std::string{ "..\0x", 4 } };
}

}

BOOST_FIXTURE_TEST_SUITE(PltBeneathTree, tree_fixture)

BOOST_AUTO_TEST_CASE(make_folders_makes_the_whole_chain_and_is_fine_when_it_exists)
{
	BOOST_TEST(platform::beneath::make_folders_beneath(pinned, "a/b/c").empty());
	BOOST_TEST(fs::is_directory(root / "a" / "b" / "c"));
	BOOST_TEST(platform::beneath::make_folders_beneath(pinned, "a/b/c").empty());
}

BOOST_AUTO_TEST_CASE(make_folders_must_be_new_takes_a_name_once)
{
	BOOST_TEST(platform::beneath::make_folders_beneath(pinned, "copies/1-0", true).empty());
	const std::string again = platform::beneath::make_folders_beneath(pinned, "copies/1-0", true);
	BOOST_TEST(again.find("already exists") != std::string::npos);
	BOOST_TEST(!platform::beneath::make_folders_beneath(pinned, "", true).empty());
}

BOOST_AUTO_TEST_CASE(make_folders_refuses_a_link_at_the_first_or_a_middle_folder)
{
	for (const bool junction : link_kinds()) {
		BOOST_TEST_INFO("junction: " << junction);
		fs::remove_all(root / "first");
		fs::remove_all(root / "a");
		if (!link_folder(outside(), root / "first", junction)) {
			BOOST_TEST_MESSAGE("links unavailable");
			continue;
		}
		BOOST_TEST(!platform::beneath::make_folders_beneath(pinned, "first/made").empty());
		fs::create_directories(root / "a");
		BOOST_REQUIRE(link_folder(outside(), root / "a" / "middle", junction));
		BOOST_TEST(!platform::beneath::make_folders_beneath(pinned, "a/middle/made", true).empty());
		BOOST_TEST(!fs::exists(outside() / "made"));
		fs::remove(root / "first");
		fs::remove(root / "a" / "middle");
	}
}

BOOST_AUTO_TEST_CASE(remove_tree_removes_nested_folders_and_files)
{
	dir.write("tree/a/b/one.txt", "1");
	dir.write("tree/a/two.txt", "2");
	fs::create_directories(root / "tree" / "empty");

	BOOST_TEST(platform::beneath::remove_tree_beneath(pinned, "", "tree").empty());
	BOOST_TEST(!fs::exists(root / "tree"));
	BOOST_TEST(platform::beneath::remove_tree_beneath(pinned, "", "tree").empty(), "nothing there is success");
}

BOOST_AUTO_TEST_CASE(remove_tree_removes_a_link_at_a_leaf_as_the_link)
{
	for (const bool junction : link_kinds()) {
		BOOST_TEST_INFO("junction: " << junction);
		dir.write("tree/a/one.txt", "1");
		if (!link_folder(outside(), root / "tree" / "a" / "leaf", junction)) {
			BOOST_TEST_MESSAGE("links unavailable");
			continue;
		}
		BOOST_TEST(platform::beneath::remove_tree_beneath(pinned, "", "tree").empty());
		BOOST_TEST(!fs::exists(root / "tree"));
		BOOST_TEST(fs::exists(outside() / "keep.txt"), "the link's target is not touched");
	}
}

BOOST_AUTO_TEST_CASE(remove_tree_refuses_a_link_as_the_tree_or_on_the_way_to_it)
{
	for (const bool junction : link_kinds()) {
		BOOST_TEST_INFO("junction: " << junction);
		if (!link_folder(outside(), root / "tree", junction)) {
			BOOST_TEST_MESSAGE("links unavailable");
			continue;
		}
		BOOST_TEST(!platform::beneath::remove_tree_beneath(pinned, "", "tree").empty());
		BOOST_TEST(!platform::beneath::remove_tree_beneath(pinned, "tree", "keep.txt").empty());
		BOOST_TEST(fs::exists(outside() / "keep.txt"));
		fs::remove(root / "tree");
	}
}

BOOST_AUTO_TEST_CASE(remove_tree_removes_a_link_swapped_in_mid_walk_as_the_link)
{
	for (const bool junction : link_kinds()) {
		BOOST_TEST_INFO("junction: " << junction);
		dir.write("tree/a/b/inside.txt", "x");
		bool swapped = false;
		// Once tree/a is held, its folder b becomes a link to the outside before a's entries are read.
		const platform::beneath::tree_hook hook = [&](const fs::path& folder) {
			if (folder.filename() != "a" || swapped) return;
			fs::remove_all(folder / "b");
			swapped = link_folder(outside(), folder / "b", junction);
		};
		BOOST_TEST(platform::beneath::remove_tree_beneath_watched(pinned, "", "tree", hook).empty());
		if (!swapped) {
			BOOST_TEST_MESSAGE("links unavailable");
			continue;
		}
		BOOST_TEST(!fs::exists(root / "tree"));
		BOOST_TEST(fs::exists(outside() / "keep.txt"), "the walk never entered the link");
	}
}

BOOST_AUTO_TEST_CASE(make_folders_refuses_dot_dot_an_absolute_path_and_a_nul)
{
	fs::create_directories(root / "inner");
	platform::beneath::pinned_root inner{ root / "inner" };
	BOOST_TEST(!platform::beneath::make_folders_beneath(inner, "../made").empty());
	BOOST_TEST(!platform::beneath::make_folders_beneath(inner, root / "made").empty());
	BOOST_TEST(!platform::beneath::make_folders_beneath(inner, dot_dot_nul() / "made").empty());
	BOOST_TEST(!platform::beneath::make_folders_beneath(inner, dot_dot_nul(), true).empty());
	BOOST_TEST(!fs::exists(root / "made"));
	BOOST_TEST(!fs::exists(root / "x"));
}

BOOST_AUTO_TEST_CASE(remove_tree_refuses_dot_dot_an_absolute_path_and_a_nul)
{
	fs::create_directories(root / "inner");
	platform::beneath::pinned_root inner{ root / "inner" };
	BOOST_TEST(!platform::beneath::remove_tree_beneath(inner, "", "..").empty());
	BOOST_TEST(!platform::beneath::remove_tree_beneath(inner, "..", "outside").empty());
	BOOST_TEST(!platform::beneath::remove_tree_beneath(inner, root, "outside").empty());
	BOOST_TEST(!platform::beneath::remove_tree_beneath(inner, "", root / "outside").empty());
	BOOST_TEST(!platform::beneath::remove_tree_beneath(inner, "", dot_dot_nul()).empty());
	BOOST_TEST(!platform::beneath::remove_tree_beneath(inner, dot_dot_nul(), "outside").empty());
	BOOST_TEST(fs::exists(outside() / "keep.txt"));
	BOOST_TEST(fs::exists(root / "inner"));
}

BOOST_AUTO_TEST_CASE(write_and_rename_refuse_a_nul_in_a_name)
{
	dir.write("inner/a.txt", "a");
	platform::beneath::pinned_root inner{ root / "inner" };
	platform::beneath::write_request write;
	write.folder = dot_dot_nul();
	write.leaf = "written.txt";
	write.tmp_leaf = "written.tmp";
	BOOST_TEST(!platform::beneath::write_beneath(inner, write).empty());
	write.folder = "";
	write.leaf = dot_dot_nul();
	BOOST_TEST(!platform::beneath::write_beneath(inner, write).empty());
	BOOST_TEST(!fs::exists(root / "written.txt"));

	platform::beneath::rename_request rename;
	rename.from_leaf = "a.txt";
	rename.to_folder = dot_dot_nul();
	rename.to_leaf = "moved.txt";
	BOOST_TEST(!platform::beneath::rename_beneath(inner, rename).empty());
	BOOST_TEST(fs::exists(root / "inner" / "a.txt"));
	BOOST_TEST(!fs::exists(root / "moved.txt"));
}

BOOST_AUTO_TEST_CASE(windows_refuses_names_it_cannot_remove_later)
{
	using platform::beneath::windows_refuses_name;
	for (const std::u16string_view name : { u"trailing.", u"trailing ", u"a:stream", u"CON", u"con", u"Nul.txt",
			 u"NUL .txt", u"aux.tar.gz", u"PRN", u"COM1", u"lpt9.log", u"COM¹", u"CONIN$", u"conout$.x" }) {
		BOOST_TEST_INFO("refused, " << name.size() << " units");
		BOOST_TEST(windows_refuses_name(name));
	}
	for (const std::u16string_view name : { u"plain.txt", u".hidden", u"CONSOLE", u"COM", u"COM10", u"LPTX", u"nul_x",
			 u"x.con", u"a b" }) {
		BOOST_TEST_INFO("allowed, " << name.size() << " units");
		BOOST_TEST(!windows_refuses_name(name));
	}
#ifdef _WIN32
	BOOST_TEST(!platform::beneath::make_folders_beneath(pinned, "made/trailing.").empty());
	BOOST_TEST(!platform::beneath::make_folders_beneath(pinned, "nul").empty());
	BOOST_TEST(!platform::beneath::remove_tree_beneath(pinned, "", "CON").empty());
#endif
}

BOOST_AUTO_TEST_CASE(remove_tree_removes_a_file_given_as_the_tree)
{
	dir.write("a/file.txt", "x");
	BOOST_TEST(platform::beneath::remove_tree_beneath(pinned, "a", "file.txt").empty());
	BOOST_TEST(!fs::exists(root / "a" / "file.txt"));
	BOOST_TEST(fs::is_directory(root / "a"));
}

// Windows: a tree this deep is past MAX_PATH for the std::filesystem calls that make and clean it up.
#ifndef _WIN32
BOOST_AUTO_TEST_CASE(remove_tree_refuses_a_tree_nested_past_the_depth_limit)
{
	fs::path deep = "tree";
	for (int level = 0; level < 260; ++level) {
		deep /= "d";
	}
	BOOST_REQUIRE(platform::beneath::make_folders_beneath(pinned, deep).empty());

	const std::string error = platform::beneath::remove_tree_beneath(pinned, "", "tree");
	BOOST_TEST(error.find("deeper than") != std::string::npos, error);
	BOOST_TEST(fs::is_directory(root / deep), "nothing on the way down was removed");
}
#endif

BOOST_AUTO_TEST_CASE(tree_entries_are_decided_as_the_folders_of_the_walk)
{
	using platform::beneath::tree_entry;
	using platform::beneath::tree_entry_of;
	constexpr std::uint32_t directory = platform::beneath::k_file_attribute_directory;
	constexpr std::uint32_t reparse = platform::beneath::k_file_attribute_reparse_point;
	constexpr std::uint32_t cloud = platform::beneath::k_reparse_tag_cloud;
	constexpr std::uint32_t junction = platform::beneath::k_reparse_tag_mount_point;
	constexpr std::uint32_t symlink = platform::beneath::k_reparse_tag_symlink;
	constexpr std::uint32_t other = 0x80000017u; // IO_REPARSE_TAG_WOF: neither a link nor a filter entered here

	for (const bool top : { true, false }) {
		BOOST_TEST_INFO("top: " << top);
		const tree_entry link = top ? tree_entry::refused : tree_entry::remove_itself;
		BOOST_TEST((tree_entry_of(directory, true, 0, top) == tree_entry::walk));
		BOOST_TEST((tree_entry_of(directory | reparse, true, cloud, top) == tree_entry::walk_through_filter));
		BOOST_TEST((tree_entry_of(directory | reparse, true, cloud | 0x3000u, top) == tree_entry::walk_through_filter));
		BOOST_TEST((tree_entry_of(directory | reparse, true, junction, top) == link));
		BOOST_TEST((tree_entry_of(directory | reparse, true, symlink, top) == link));
		BOOST_TEST((tree_entry_of(directory | reparse, true, other, top) == link));
		BOOST_TEST((tree_entry_of(directory | reparse, false, 0, top) == link));
		BOOST_TEST((tree_entry_of(0, true, 0, top) == tree_entry::remove_itself));
		BOOST_TEST((tree_entry_of(reparse, true, cloud, top) == tree_entry::remove_itself));
		BOOST_TEST((tree_entry_of(reparse, true, symlink, top) == link));
		BOOST_TEST((tree_entry_of(reparse, true, other, top) == link));
	}
}

#ifdef _WIN32
BOOST_AUTO_TEST_CASE(remove_tree_removes_a_read_only_file)
{
	dir.write("tree/locked.txt", "x");
	const auto write = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
	fs::permissions(root / "tree" / "locked.txt", write, fs::perm_options::remove);

	BOOST_TEST(platform::beneath::remove_tree_beneath(pinned, "", "tree").empty());
	BOOST_TEST(!fs::exists(root / "tree"));
}
#endif

#ifdef _WIN32
// #1316 (from remove_tree_without_following, #1099): a read-only junction goes as the link; its read-only target stays
// read-only and whole.
BOOST_AUTO_TEST_CASE(remove_tree_removes_a_read_only_junction_and_leaves_its_target_as_it_was)
{
	const fs::path target = outside();
	BOOST_REQUIRE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY));
	fs::create_directories(root / "tree");
	const fs::path link = root / "tree" / "escape";
	BOOST_REQUIRE(link_folder(target, link, true));
	BOOST_REQUIRE(set_read_only(link));

	const std::string error = platform::beneath::remove_tree_beneath(pinned, "", "tree");
	BOOST_TEST(error.empty(), error);
	BOOST_TEST(!fs::exists(root / "tree"));
	BOOST_TEST(fs::is_regular_file(target / "keep.txt"));
	const DWORD attributes = GetFileAttributesW(target.c_str());
	BOOST_TEST((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0));
	SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_DIRECTORY);
}

// #1316 (from remove_tree_without_following, #1099): a tree whose paths run past MAX_PATH goes whole.
BOOST_AUTO_TEST_CASE(remove_tree_removes_a_tree_deeper_than_max_path)
{
	std::wstring deep = L"\\\\?\\" + (root / "tree").lexically_normal().make_preferred().wstring();
	while (deep.size() < MAX_PATH + 60) {
		deep += L"\\a_folder_name_of_some_length";
	}
	std::error_code made;
	fs::create_directories(fs::path(deep), made);
	BOOST_REQUIRE_MESSAGE(!made, made.message());
	const HANDLE file = CreateFileW((deep + L"\\file.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_READONLY, nullptr);
	BOOST_REQUIRE(file != INVALID_HANDLE_VALUE);
	CloseHandle(file);

	const std::string error = platform::beneath::remove_tree_beneath(pinned, "", "tree");
	BOOST_TEST(error.empty(), error);
	BOOST_TEST(!fs::exists(root / "tree"));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
