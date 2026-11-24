#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_beneath.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// #1187: a folder below a root listed without following a link and without making anything.
namespace {

namespace fs = std::filesystem;
using platform::beneath::entry_kind;

void touch(const fs::path& file)
{
	std::ofstream(file, std::ios::binary) << "x";
}

entry_kind kind_of(const platform::beneath::list_result& listed, const std::string& name)
{
	const auto found = std::ranges::find_if(listed.entries, [&](const auto& entry) { return entry.name == fs::path(name); });
	BOOST_REQUIRE_MESSAGE(found != listed.entries.end(), name << " is not listed");
	return found->kind;
}

}

BOOST_AUTO_TEST_SUITE(PltBeneathList)

BOOST_AUTO_TEST_CASE(ListsFilesAndFoldersWithTheirKinds)
{
	test_util::temp_dir dir("beneath_list");
	fs::create_directories(dir.path() / "autosave" / "inner");
	touch(dir.path() / "autosave" / "a.json");
	touch(dir.path() / "autosave" / "b");
	platform::beneath::pinned_root root(dir.path());

	const auto listed = platform::beneath::list_beneath(root, "autosave", 100);
	BOOST_TEST(listed.error.empty());
	BOOST_TEST(!listed.missing);
	BOOST_TEST(!listed.truncated);
	BOOST_TEST(listed.entries.size() == 3u);
	BOOST_TEST((kind_of(listed, "a.json") == entry_kind::file));
	BOOST_TEST((kind_of(listed, "b") == entry_kind::file));
	BOOST_TEST((kind_of(listed, "inner") == entry_kind::folder));

	const auto top = platform::beneath::list_beneath(root, "", 100);
	BOOST_TEST(top.error.empty());
	BOOST_TEST((kind_of(top, "autosave") == entry_kind::folder));
}

BOOST_AUTO_TEST_CASE(StopsAfterTheLimit)
{
	test_util::temp_dir dir("beneath_list_limit");
	fs::create_directories(dir.path() / "many");
	for (int i = 0; i < 10; ++i) {
		touch(dir.path() / "many" / ("f" + std::to_string(i)));
	}
	platform::beneath::pinned_root root(dir.path());
	const auto listed = platform::beneath::list_beneath(root, "many", 4);
	BOOST_TEST(listed.error.empty());
	BOOST_TEST(listed.truncated);
	BOOST_TEST(listed.entries.size() == 4u);

	const auto exact = platform::beneath::list_beneath(root, "many", 10);
	BOOST_TEST(!exact.truncated);
	BOOST_TEST(exact.entries.size() == 10u);
}

BOOST_AUTO_TEST_CASE(AMissingFolderOrRoot_IsMissing_AndNothingIsMade)
{
	test_util::temp_dir dir("beneath_list_missing");
	platform::beneath::pinned_root root(dir.path());
	const auto folder = platform::beneath::list_beneath(root, fs::path("autosave") / "nothing", 10);
	BOOST_TEST(folder.missing);
	BOOST_TEST(folder.error.empty());
	BOOST_TEST(!fs::exists(dir.path() / "autosave"));

	platform::beneath::pinned_root absent(dir.path() / "no_root");
	const auto rootless = platform::beneath::list_beneath(absent, "autosave", 10);
	BOOST_TEST(rootless.missing);
	BOOST_TEST(!fs::exists(dir.path() / "no_root"));
}

BOOST_AUTO_TEST_CASE(APathThatLeavesTheRoot_IsRefused)
{
	test_util::temp_dir dir("beneath_list_refused");
	platform::beneath::pinned_root root(dir.path());
	for (const char* folder : { "../x", "a/../../x" }) {
		BOOST_TEST_CONTEXT(folder) {
			const auto listed = platform::beneath::list_beneath(root, folder, 10);
			BOOST_TEST(!listed.error.empty());
			BOOST_TEST(listed.entries.empty());
		}
	}
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(ALinkIsListedAsALink_AndNotFollowed)
{
	test_util::temp_dir dir("beneath_list_links");
	test_util::temp_dir outside("beneath_list_outside");
	touch(outside.path() / "secret");
	fs::create_directories(dir.path() / "autosave");
	fs::create_directory_symlink(outside.path(), dir.path() / "autosave" / "to_folder");
	fs::create_symlink(outside.path() / "secret", dir.path() / "autosave" / "to_file");
	fs::create_directory_symlink(outside.path(), dir.path() / "linked");
	platform::beneath::pinned_root root(dir.path());

	const auto listed = platform::beneath::list_beneath(root, "autosave", 10);
	BOOST_TEST(listed.error.empty());
	BOOST_TEST((kind_of(listed, "to_folder") == entry_kind::link));
	BOOST_TEST((kind_of(listed, "to_file") == entry_kind::link));

	const auto through = platform::beneath::list_beneath(root, "linked", 10);
	BOOST_TEST(!through.error.empty());
	BOOST_TEST(!through.missing);
	BOOST_TEST(through.entries.empty());
}

BOOST_AUTO_TEST_CASE(ANameThatIsNotUtf8_IsListedWithoutAnError)
{
	test_util::temp_dir dir("beneath_list_bytes");
	fs::create_directories(dir.path() / "autosave");
	const std::string name = std::string("bad") + static_cast<char>(0xff) + static_cast<char>(0xfe);
	touch(dir.path() / "autosave" / name);
	platform::beneath::pinned_root root(dir.path());
	const auto listed = platform::beneath::list_beneath(root, "autosave", 10);
	BOOST_TEST(listed.error.empty());
	BOOST_REQUIRE(listed.entries.size() == 1u);
	BOOST_TEST(listed.entries.front().name.native() == name);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
