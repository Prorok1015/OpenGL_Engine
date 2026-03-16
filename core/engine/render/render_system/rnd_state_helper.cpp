#include "rnd_state_helper.h"

std::string rnd::to_string(rnd::driver::RENDER_MODE mode)
{
	using namespace rnd::driver;
	switch (mode) {
	case RENDER_MODE::TRIANGLE: return "TRIANGLE";
	case RENDER_MODE::TRIANGLE_STRIP: return "TRIANGLE_STRIP";
	case RENDER_MODE::TRIANGLE_FAN: return "TRIANGLE_FAN";
	case RENDER_MODE::TRIANGLE_ADJ: return "TRIANGLE_ADJ";
	case RENDER_MODE::TRIANGLE_STRIP_ADJ: return "TRIANGLE_STRIP_ADJ";
	case RENDER_MODE::LINE: return "LINE";
	case RENDER_MODE::LINE_STRIP: return "LINE_STRIP";
	case RENDER_MODE::LINE_LOOP: return "LINE_LOOP";
	case RENDER_MODE::LINE_ADJ: return "LINE_ADJ";
	case RENDER_MODE::LINE_STRIP_ADJ: return "LINE_STRIP_ADJ";
	case RENDER_MODE::POINT: return "POINT";
	}

	return "UNKNOWN";
}

rnd::driver::RENDER_MODE rnd::render_mode_from_string(const std::string& str)
{
	using namespace rnd::driver;
	if (str == "TRIANGLE") return RENDER_MODE::TRIANGLE;
	if (str == "TRIANGLE_STRIP") return RENDER_MODE::TRIANGLE_STRIP;
	if (str == "TRIANGLE_FAN") return RENDER_MODE::TRIANGLE_FAN;
	if (str == "TRIANGLE_ADJ") return RENDER_MODE::TRIANGLE_ADJ;
	if (str == "TRIANGLE_STRIP_ADJ") return RENDER_MODE::TRIANGLE_STRIP_ADJ;
	if (str == "LINE") return RENDER_MODE::LINE;
	if (str == "LINE_STRIP") return RENDER_MODE::LINE_STRIP;
	if (str == "LINE_LOOP") return RENDER_MODE::LINE_LOOP;
	if (str == "LINE_ADJ") return RENDER_MODE::LINE_ADJ;
	if (str == "LINE_STRIP_ADJ") return RENDER_MODE::LINE_STRIP_ADJ;
	if (str == "POINT") return RENDER_MODE::POINT;
	return RENDER_MODE::TRIANGLE; // Default
}