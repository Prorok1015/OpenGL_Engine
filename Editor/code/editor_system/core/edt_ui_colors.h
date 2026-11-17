#pragma once
#include <imgui.h>

namespace edt
{
	// `c` with its rgb scaled by `factor`, alpha kept: the shades of a button drawn in a semantic colour (#1136), so a
	// panel derives them from the theme instead of writing new literals.
	inline ImVec4 shade(ImVec4 c, float factor)
	{
		return ImVec4(c.x * factor, c.y * factor, c.z * factor, c.w);
	}
}
