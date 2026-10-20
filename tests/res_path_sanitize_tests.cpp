#include <boost/test/unit_test.hpp>
#include <snk/resource/res_path_sanitize.h>

BOOST_AUTO_TEST_SUITE(ResPathSanitizeTests)

BOOST_AUTO_TEST_CASE(KeepsSafeRelativePaths)
{
	BOOST_TEST(res::sanitize_relative_path("x.png") == "x.png");
	BOOST_TEST(res::sanitize_relative_path("tex/x.png") == "tex/x.png");
	BOOST_TEST(res::sanitize_relative_path("tex\\sub\\x.png") == "tex/sub/x.png");
	BOOST_TEST(res::sanitize_relative_path("./tex//x.png") == "tex/x.png");
}

BOOST_AUTO_TEST_CASE(DropsParentRootAndDriveParts)
{
	BOOST_TEST(res::sanitize_relative_path("../../x.png") == "x.png");
	BOOST_TEST(res::sanitize_relative_path("a/../../x.png") == "a/x.png");
	BOOST_TEST(res::sanitize_relative_path("/abs/x.png") == "abs/x.png");
	BOOST_TEST(res::sanitize_relative_path("C:\\x.png") == "x.png");
	BOOST_TEST(res::sanitize_relative_path("C:/dir/x.png") == "dir/x.png");
	BOOST_TEST(res::sanitize_relative_path("\\\\server\\share\\x.png") == "server/share/x.png");
}

BOOST_AUTO_TEST_CASE(NeverProducesProtocolOrEmptyResult)
{
	BOOST_TEST(res::sanitize_relative_path("http://host/x.png") == "http_/host/x.png");
	BOOST_TEST(res::sanitize_relative_path("").size() > 0u);
	BOOST_TEST(res::sanitize_relative_path("..") == res::sanitize_relative_path(""));
	BOOST_TEST(res::sanitize_relative_path("../..\\") == res::sanitize_relative_path(""));
}

BOOST_AUTO_TEST_SUITE_END()
