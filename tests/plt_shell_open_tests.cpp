#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_shell_open.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <string>

// open_in_shell refuses what is not an absolute path to a file before it starts anything; opening for real needs a
// desktop, so it is not tried here.
BOOST_AUTO_TEST_SUITE(PltShellOpen)

BOOST_AUTO_TEST_CASE(RelativePath_IsRefused)
{
	const std::string error = platform::open_in_shell("src/game.cpp");
	BOOST_TEST(error.find("not an absolute path") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissingFile_IsRefused)
{
	test_util::temp_dir dir("plt_shell_open_missing");
	const std::string error = platform::open_in_shell(dir.path() / "missing.cpp");
	BOOST_TEST(error.find("not an existing file") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Directory_IsRefused)
{
	test_util::temp_dir dir("plt_shell_open_dir");
	const std::string error = platform::open_in_shell(dir.path());
	BOOST_TEST(error.find("not an existing file") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
