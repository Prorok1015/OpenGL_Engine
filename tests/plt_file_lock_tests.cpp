#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_file_lock.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

BOOST_AUTO_TEST_SUITE(PltFileLock)

// #760: one holder at a time, also within one process; the lock is free again once its holder goes.
BOOST_AUTO_TEST_CASE(one_holder_at_a_time)
{
	test_util::temp_dir dir("file_lock");
	const std::filesystem::path relative = std::filesystem::path("sub") / ".lock";
	std::optional<platform::file_lock> first = platform::file_lock::try_acquire(dir.path(), relative);
	BOOST_REQUIRE(first.has_value());
	BOOST_TEST(!platform::file_lock::try_acquire(dir.path(), relative).has_value());

	std::optional<platform::file_lock> moved = std::move(first);
	first.reset();
	BOOST_TEST(!platform::file_lock::try_acquire(dir.path(), relative).has_value());

	moved.reset();
	BOOST_TEST(platform::file_lock::try_acquire(dir.path(), relative).has_value());
}

BOOST_AUTO_TEST_CASE(a_path_that_is_not_plain_names_is_refused)
{
	test_util::temp_dir dir("file_lock_names");
	for (const char* relative : { "", "../.lock", "sub/../.lock", "./.lock" }) {
		BOOST_TEST_CONTEXT(relative) {
			std::string error;
			BOOST_TEST(!platform::file_lock::try_acquire(dir.path(), relative, &error).has_value());
			BOOST_TEST(!error.empty());
		}
	}
}

#if !defined(_WIN32)
// #822: a project could hold a link where the lock goes; its target must not be made, opened or changed.
BOOST_AUTO_TEST_CASE(a_link_at_the_file_or_a_folder_is_not_followed)
{
	test_util::temp_dir dir("file_lock_links");
	test_util::temp_dir outside("file_lock_outside");
	const std::filesystem::path target = outside.path() / "victim";
	{
		std::ofstream(target, std::ios::binary) << "keep";
	}
	std::filesystem::create_directories(dir.path() / "cache");
	std::filesystem::create_symlink(target, dir.path() / "cache" / ".lock");
	std::filesystem::create_directory_symlink(outside.path(), dir.path() / "linked");

	std::string error;
	BOOST_TEST(!platform::file_lock::try_acquire(dir.path(), std::filesystem::path("cache") / ".lock", &error).has_value());
	BOOST_TEST(!error.empty());
	error.clear();
	BOOST_TEST(!platform::file_lock::try_acquire(dir.path(), std::filesystem::path("linked") / ".lock", &error).has_value());
	BOOST_TEST(!error.empty());
	BOOST_TEST(!std::filesystem::exists(outside.path() / ".lock"));
	std::ifstream in(target, std::ios::binary);
	BOOST_TEST(std::string(std::istreambuf_iterator<char>(in), {}) == "keep");
}
#else
// #822: a junction at a folder of the lock's path is refused and nothing is made where it leads.
BOOST_AUTO_TEST_CASE(a_junction_at_a_folder_is_not_followed)
{
	test_util::temp_dir dir("file_lock_junction");
	test_util::temp_dir outside("file_lock_junction_outside");
	const std::filesystem::path link = dir.path() / "linked";
	const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + outside.path().string() + "\" >NUL";
	BOOST_REQUIRE_EQUAL(std::system(command.c_str()), 0);
	BOOST_REQUIRE(std::filesystem::exists(link));

	std::string error;
	BOOST_TEST(!platform::file_lock::try_acquire(dir.path(), std::filesystem::path("linked") / ".lock", &error).has_value());
	BOOST_TEST(!error.empty());
	BOOST_TEST(!std::filesystem::exists(outside.path() / ".lock"));
	std::filesystem::remove(link);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
