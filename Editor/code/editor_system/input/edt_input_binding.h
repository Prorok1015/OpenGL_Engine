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

	// The full-window Game view (#294) feeds the game unless something is asked of the person over it: an ImGui popup
	// or modal, or an editor dialog ImGui does not see as one (`dialog_open`).
	constexpr bool game_input_allowed(bool popup_open, bool dialog_open)
	{
		return !popup_open && !dialog_open;
	}

	// Binds the rect `pos`/`size` (window units) to `target` for this frame, hovered or not, when game_input_allowed.
	// True when bound.
	inline bool bind_game_window_input(inp::input_router* router, const std::optional<inp::input_target>& target,
		glm::vec2 pos, glm::vec2 size, bool dialog_open)
	{
		const bool popup_open = ImGui::IsPopupOpen(static_cast<const char*>(nullptr), ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
		if (!router || !target || !game_input_allowed(popup_open, dialog_open)) return false;
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		router->bind(*target, make_input_area(pos, size, { fb_scale.x, fb_scale.y }));
		return true;
	}
}
