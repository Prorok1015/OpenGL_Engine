#include <boost/test/unit_test.hpp>
#include "edt_ui_probe.h"
#include <string>
#include <vector>

// The item rectangles the headless smoke scripts read from the editor log (#646).

namespace {

struct probe_fixture
{
	std::vector<std::string> lines;
	edt::ui_probe on{ true, [this](const std::string& line) { lines.push_back(line); } };
	edt::ui_probe off{ false, [this](const std::string& line) { lines.push_back(line); } };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtUiProbeTests, probe_fixture)

BOOST_AUTO_TEST_CASE(ALineNamesTheAreaTheItemAndTheCentreOfItsRectangle)
{
	on.report("asset_browser", "base_material.desc", 500.f, 1018.f, 720.f, 1036.f);
	BOOST_REQUIRE_EQUAL(lines.size(), 1u);
	BOOST_CHECK_EQUAL(lines[0], "item asset_browser 'base_material.desc' center 610 1027 size 220 18");
}

BOOST_AUTO_TEST_CASE(ARectangleThatDoesNotChange_IsNotReportedAgain)
{
	for (int frame = 0; frame < 5; ++frame) {
		on.report("asset_browser", "a.desc", 10.f, 20.f, 110.f, 37.f);
	}
	BOOST_CHECK_EQUAL(lines.size(), 1u);
}

BOOST_AUTO_TEST_CASE(AMovedItem_IsReportedAgain_AndTheLastLineIsWhereItIs)
{
	on.report("asset_browser", "a.desc", 10.f, 20.f, 110.f, 37.f);
	on.report("asset_browser", "a.desc", 10.f, 37.f, 110.f, 54.f);
	BOOST_REQUIRE_EQUAL(lines.size(), 2u);
	BOOST_CHECK_EQUAL(lines.back(), "item asset_browser 'a.desc' center 60 46 size 100 17");
}

BOOST_AUTO_TEST_CASE(ItemsOfOtherNamesAndAreas_AreTrackedApart)
{
	on.report("asset_browser", "a.desc", 10.f, 20.f, 110.f, 37.f);
	on.report("asset_browser", "b.desc", 10.f, 20.f, 110.f, 37.f);
	on.report("hierarchy", "a.desc", 10.f, 20.f, 110.f, 37.f);
	BOOST_CHECK_EQUAL(lines.size(), 3u);
}

BOOST_AUTO_TEST_CASE(WithoutTheFlag_NothingIsWritten_HoweverOftenItIsAsked)
{
	BOOST_CHECK(!off.enabled());
	for (int frame = 0; frame < 5; ++frame) {
		off.report("asset_browser", "a.desc", 10.f, static_cast<float>(frame), 110.f, 37.f);
	}
	BOOST_CHECK(lines.empty());
}

BOOST_AUTO_TEST_SUITE_END()
