#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_beneath.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// #1294: folders made and trees removed below a root without going through a link.
namespace {

namespace fs = std::filesystem;

// A link to a folder at `link`: a directory symlink, or on Windows a junction when `junction`. False when this machine
// cannot make one (Windows without the symlink privilege); the test then has nothing to check.
bool link_folder(const fs::path& target, const fs::path& link, bool junction = false)
{
#ifdef _WIN32
	if (junction) {
		const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
		return std::system(command.c_str()) == 0 && fs::exists(link);
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
		BOOST_TEST(platform::beneath::remove_tree_beneath(pinned, "", "tree", &hook).empty());
		if (!swapped) {
			BOOST_TEST_MESSAGE("links unavailable");
			continue;
		}
		BOOST_TEST(!fs::exists(root / "tree"));
		BOOST_TEST(fs::exists(outside() / "keep.txt"), "the walk never entered the link");
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

BOOST_AUTO_TEST_SUITE_END()
