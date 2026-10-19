#pragma once
#include "edt_input_manager.h"
#include <snk/input/inp_input_router.h>
#include <imgui.h>
#include <functional>
#include <optional>

namespace edt
{
	// Which context a panel feeds with input; empty when there is none (no level loaded).
	using input_target_getter = std::function<std::optional<inp::input_target>()>;

	// Binds the item submitted last to `target` for this frame. Only a hovered item binds: ImGui is
	// the one that knows a floating window or a popup covers the image, a bare rect would not.
	inline void bind_item_input(inp::input_router* router, const std::optional<inp::input_target>& target)
	{
		if (!router || !target || !ImGui::IsItemHovered()) return;

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		router->bind(*target, make_input_area({ min.x, min.y }, { max.x - min.x, max.y - min.y }, { fb_scale.x, fb_scale.y }));
	}
}
