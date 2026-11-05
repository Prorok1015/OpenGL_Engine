#pragma once
#include "edt_input_manager.h"
#include <snk/input/inp_input_router.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <optional>

namespace edt
{
	// Which context a panel feeds with input; empty when there is none (no level loaded).
	using input_target_getter = std::function<std::optional<inp::input_target>()>;

	inline void bind_rect_input(inp::input_router& router, const inp::input_target& target, ImVec2 min, ImVec2 max)
	{
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		router.bind(target, make_input_area({ min.x, min.y }, { max.x - min.x, max.y - min.y }, { fb_scale.x, fb_scale.y }));
	}

	// Binds the item submitted last to `target` for this frame. Only a hovered item binds: ImGui is
	// the one that knows a floating window or a popup covers the image, a bare rect would not. True when bound.
	inline bool bind_item_input(inp::input_router* router, const std::optional<inp::input_target>& target)
	{
		if (!router || !target || !ImGui::IsItemHovered()) return false;
		bind_rect_input(*router, *target, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		return true;
	}

	// The image of a view, as it was when it was drawn: its rect and whether the mouse is over it.
	struct view_item
	{
		ImVec2 min{ 0.f, 0.f };
		ImVec2 max{ 0.f, 0.f };
		bool hovered = false;
	};

	// Call right after the image is submitted.
	inline view_item capture_view_item()
	{
		return { ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::IsItemHovered() };
	}

	// An item other than the window itself is held. A press on an item without an id (the image) makes ImGui move the
	// window, and holds that as the active id (the window's MoveId, or its TabId when docked) until release: that is the
	// person dragging in the view, not a handle.
	inline bool another_item_is_active()
	{
		const ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
		return g.ActiveId != 0 && (!window || (g.ActiveId != window->MoveId && g.ActiveId != window->TabId));
	}

	// Binds the image of a view once everything that sits on top of it is submitted (#368, #545). The image itself has no
	// ImGui id, so ImGui calls it hovered also while the mouse is over a gizmo handle drawn above it, and a handle that is
	// held leaves it hovered on the frame of the press; the world must not get that mouse. So: no binding while any item is
	// under or held by the mouse. `blocked` is a drag the view tracks itself (a rotate ring). True when bound.
	inline bool bind_view_input(inp::input_router* router, const std::optional<inp::input_target>& target,
		const view_item& view, bool blocked = false)
	{
		if (!router || !target || !view.hovered || blocked) return false;
		if (ImGui::IsAnyItemHovered() || another_item_is_active()) return false;
		bind_rect_input(*router, *target, view.min, view.max);
		return true;
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
