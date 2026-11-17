#include <snk/render/rnd_state_helper.h>

std::string rnd::to_string(rnd::driver::RENDER_MODE mode)
{
	using namespace rnd::driver;
	switch (mode) {
	case RENDER_MODE::TRIANGLE: return "TRIANGLE";
	case RENDER_MODE::TRIANGLE_STRIP: return "TRIANGLE_STRIP";
	case RENDER_MODE::LINE: return "LINE";
	case RENDER_MODE::LINE_STRIP: return "LINE_STRIP";
	case RENDER_MODE::POINT: return "POINT";
	}

	return "UNKNOWN";
}

rnd::driver::RENDER_MODE rnd::render_mode_from_string(const std::string& str)
{
	using namespace rnd::driver;
	if (str == "TRIANGLE") return RENDER_MODE::TRIANGLE;
	if (str == "TRIANGLE_STRIP") return RENDER_MODE::TRIANGLE_STRIP;
	if (str == "LINE") return RENDER_MODE::LINE;
	if (str == "LINE_STRIP") return RENDER_MODE::LINE_STRIP;
	if (str == "POINT") return RENDER_MODE::POINT;
	return RENDER_MODE::TRIANGLE; // Default
}