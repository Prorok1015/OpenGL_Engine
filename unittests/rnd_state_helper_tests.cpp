#include <boost/test/unit_test.hpp>
#include "rnd_state_helper.h"

using namespace rnd::driver;

BOOST_AUTO_TEST_SUITE(RndStateHelperTests)

// --- to_string ---

BOOST_AUTO_TEST_CASE(ToString_Triangle) {
	BOOST_TEST(rnd::to_string(RENDER_MODE::TRIANGLE) == "TRIANGLE");
}

BOOST_AUTO_TEST_CASE(ToString_TriangleStrip) {
	BOOST_TEST(rnd::to_string(RENDER_MODE::TRIANGLE_STRIP) == "TRIANGLE_STRIP");
}

BOOST_AUTO_TEST_CASE(ToString_Line) {
	BOOST_TEST(rnd::to_string(RENDER_MODE::LINE) == "LINE");
}

BOOST_AUTO_TEST_CASE(ToString_Point) {
	BOOST_TEST(rnd::to_string(RENDER_MODE::POINT) == "POINT");
}

// --- render_mode_from_string ---

BOOST_AUTO_TEST_CASE(FromString_Triangle) {
	BOOST_CHECK(rnd::render_mode_from_string("TRIANGLE") == RENDER_MODE::TRIANGLE);
}

BOOST_AUTO_TEST_CASE(FromString_LineStrip) {
	BOOST_CHECK(rnd::render_mode_from_string("LINE_STRIP") == RENDER_MODE::LINE_STRIP);
}

BOOST_AUTO_TEST_CASE(FromString_Unknown_DefaultsToTriangle) {
	BOOST_CHECK(rnd::render_mode_from_string("INVALID") == RENDER_MODE::TRIANGLE);
}

// --- Roundtrip ---

BOOST_AUTO_TEST_CASE(Roundtrip_AllModes) {
	RENDER_MODE modes[] = {
		RENDER_MODE::TRIANGLE,
		RENDER_MODE::TRIANGLE_STRIP,
		RENDER_MODE::TRIANGLE_FAN,
		RENDER_MODE::TRIANGLE_ADJ,
		RENDER_MODE::TRIANGLE_STRIP_ADJ,
		RENDER_MODE::LINE,
		RENDER_MODE::LINE_STRIP,
		RENDER_MODE::LINE_LOOP,
		RENDER_MODE::LINE_ADJ,
		RENDER_MODE::LINE_STRIP_ADJ,
		RENDER_MODE::POINT
	};
	for (auto mode : modes) {
		auto str = rnd::to_string(mode);
		auto result = rnd::render_mode_from_string(str);
		BOOST_CHECK(result == mode);
	}
}

BOOST_AUTO_TEST_SUITE_END()
