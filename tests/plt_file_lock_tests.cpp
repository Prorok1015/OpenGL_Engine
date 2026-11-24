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

// #1187: another holder is `held`, not an error; once it goes, the lock is taken.
BOOST_AUTO_TEST_CASE(acquire_tells_a_held_lock_from_a_free_one)
{
	test_util::temp_dir dir("file_lock_acquire");
	const std::filesystem::path relative = std::filesystem::path("session") / "session.lock";
	platform::lock_attempt first = platform::file_lock::acquire(dir.path(), relative);
	BOOST_REQUIRE((first.outcome == platform::lock_outcome::acquired));
	BOOST_TEST(first.lock.has_value());

	const platform::lock_attempt second = platform::file_lock::acquire(dir.path(), relative);
	BOOST_TEST((second.outcome == platform::lock_outcome::held));
	BOOST_TEST(!second.lock.has_value());
	BOOST_TEST(second.error.empty());
	BOOST_TEST((platform::file_lock::acquire(dir.path(), relative, platform::lock_open::existing_only).outcome
		== platform::lock_outcome::held));

	first.lock.reset();
	const platform::lock_attempt probe = platform::file_lock::acquire(dir.path(), relative, platform::lock_open::existing_only);
	BOOST_TEST((probe.outcome == platform::lock_outcome::acquired));
}

// #1187: a probe of a lock file that is not there makes neither the file nor its folders nor the root.
BOOST_AUTO_TEST_CASE(a_probe_makes_nothing)
{
	test_util::temp_dir dir("file_lock_probe");
	const std::filesystem::path root = dir.path() / "cache";
	const std::filesystem::path relative = std::filesystem::path("session") / "session.lock";
	BOOST_TEST((platform::file_lock::acquire(root, relative, platform::lock_open::existing_only).outcome
		== platform::lock_outcome::missing));
	BOOST_TEST(!std::filesystem::exists(root));

	std::filesystem::create_directories(root);
	BOOST_TEST((platform::file_lock::acquire(root, relative, platform::lock_open::existing_only).outcome
		== platform::lock_outcome::missing));
	BOOST_TEST(!std::filesystem::exists(root / "session"));

	std::filesystem::create_directories(root / "session");
	BOOST_TEST((platform::file_lock::acquire(root, relative, platform::lock_open::existing_only).outcome
		== platform::lock_outcome::missing));
	BOOST_TEST(!std::filesystem::exists(root / relative));
}

BOOST_AUTO_TEST_CASE(acquire_refuses_a_path_that_is_not_plain_names)
{
	test_util::temp_dir dir("file_lock_acquire_names");
	const platform::lock_attempt attempt = platform::file_lock::acquire(dir.path(), "../.lock");
	BOOST_TEST((attempt.outcome == platform::lock_outcome::failed));
	BOOST_TEST(!attempt.error.empty());
}

#if !defined(_WIN32)
// #1187: a probe through a link is refused as an error, never taken for a missing or a free lock.
BOOST_AUTO_TEST_CASE(a_probe_through_a_link_fails)
{
	test_util::temp_dir dir("file_lock_probe_link");
	test_util::temp_dir outside("file_lock_probe_outside");
	{
		std::ofstream(outside.path() / "session.lock", std::ios::binary) << "";
	}
	std::filesystem::create_directory_symlink(outside.path(), dir.path() / "session");
	const platform::lock_attempt attempt = platform::file_lock::acquire(dir.path(), std::filesystem::path("session") / "session.lock",
		platform::lock_open::existing_only);
	BOOST_TEST((attempt.outcome == platform::lock_outcome::failed));
	BOOST_TEST(!attempt.error.empty());
}
#endif

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
