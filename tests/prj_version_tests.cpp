#include <boost/test/unit_test.hpp>
#include <snk/project/prj_version.h>
#include <string_view>

namespace {
	prj::engine_version v(int major, int minor, int patch)
	{
		return prj::engine_version{ major, minor, patch };
	}
}

BOOST_AUTO_TEST_SUITE(PrjVersionTests)

BOOST_AUTO_TEST_CASE(Parse_AcceptsStrictTriples)
{
	BOOST_TEST((prj::parse_engine_version("0.2.0") == v(0, 2, 0)));
	BOOST_TEST((prj::parse_engine_version("10.20.30") == v(10, 20, 30)));
}

BOOST_AUTO_TEST_CASE(Parse_RejectsEverythingElse)
{
	for (const std::string_view text : { "", "1", "1.2", "1.2.3.4", "1.2.", ".1.2", "1..2", "v1.2.3", "1.2.3-rc1",
			"-1.2.3", "1.+2.3", " 1.2.3", "1.2.3 ", "1.2.x", "9999999999.0.0" }) {
		BOOST_TEST_INFO("text: '" << text << "'");
		BOOST_TEST(!prj::parse_engine_version(text).has_value());
	}
}

BOOST_AUTO_TEST_CASE(ToString_RoundTrips)
{
	BOOST_TEST(prj::to_string(v(0, 2, 7)) == "0.2.7");
	BOOST_TEST((prj::parse_engine_version(prj::to_string(v(3, 14, 15))) == v(3, 14, 15)));
}

BOOST_AUTO_TEST_CASE(CurrentEngineVersion_IsTheSdkVersion)
{
	// SNK_SDK_VERSION is never 0.0.0; that value would mean the define did not parse.
	BOOST_TEST((prj::current_engine_version() != v(0, 0, 0)));
}

BOOST_AUTO_TEST_CASE(Compare_SameMajorMinor)
{
	BOOST_TEST((prj::compare_engine_versions(v(0, 2, 0), v(0, 2, 0)) == prj::version_match::exact));
	BOOST_TEST((prj::compare_engine_versions(v(0, 2, 1), v(0, 2, 0)) == prj::version_match::patch_differs));
	BOOST_TEST((prj::compare_engine_versions(v(0, 2, 0), v(0, 2, 5)) == prj::version_match::patch_differs));
}

BOOST_AUTO_TEST_CASE(Compare_MinorCountsAsBreakingBeforeOne)
{
	BOOST_TEST((prj::compare_engine_versions(v(0, 1, 9), v(0, 2, 0)) == prj::version_match::project_older));
	BOOST_TEST((prj::compare_engine_versions(v(0, 3, 0), v(0, 2, 9)) == prj::version_match::project_newer));
}

BOOST_AUTO_TEST_CASE(Compare_MajorAndMinorAfterOne)
{
	BOOST_TEST((prj::compare_engine_versions(v(1, 0, 0), v(2, 0, 0)) == prj::version_match::project_older));
	BOOST_TEST((prj::compare_engine_versions(v(2, 1, 0), v(2, 0, 3)) == prj::version_match::project_newer));
	BOOST_TEST((prj::compare_engine_versions(v(1, 9, 9), v(2, 0, 0)) == prj::version_match::project_older));
	BOOST_TEST((prj::compare_engine_versions(v(3, 0, 0), v(2, 9, 9)) == prj::version_match::project_newer));
}

BOOST_AUTO_TEST_SUITE_END()
