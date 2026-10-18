#pragma once
#include <glm/glm.hpp>

namespace wnd
{
	// Window units -> framebuffer pixels ratio; 1 at 100% DPI, 2 on a 2x retina display.
	inline glm::vec2 framebuffer_scale(glm::ivec2 framebuffer_size, glm::ivec2 window_size)
	{
		if (window_size.x <= 0 || window_size.y <= 0) {
			return glm::vec2{ 1.f };
		}
		return glm::vec2{ framebuffer_size } / glm::vec2{ window_size };
	}

	inline glm::vec2 to_framebuffer(glm::vec2 window_pos, glm::vec2 scale)
	{
		return window_pos * scale;
	}
}
