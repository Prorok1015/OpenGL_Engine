#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_file_lock.h>
#include <optional>

BOOST_AUTO_TEST_SUITE(PltFileLock)

// #760: one holder at a time, also within one process; the lock is free again once its holder goes.
BOOST_AUTO_TEST_CASE(one_holder_at_a_time)
{
	test_util::temp_dir dir("file_lock");
	const auto path = dir.path() / "sub" / ".lock";
	std::optional<platform::file_lock> first = platform::file_lock::try_acquire(path);
	BOOST_REQUIRE(first.has_value());
	BOOST_TEST(!platform::file_lock::try_acquire(path).has_value());

	std::optional<platform::file_lock> moved = std::move(first);
	first.reset();
	BOOST_TEST(!platform::file_lock::try_acquire(path).has_value());

	moved.reset();
	BOOST_TEST(platform::file_lock::try_acquire(path).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
