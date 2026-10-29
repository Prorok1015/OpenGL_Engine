#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_panel_registry.h>
// ImGuiID is an imgui integer typedef (unsigned int). Re-declared here to keep <imgui.h>
// out of this header (EPIC-49 US-49-4); the .cpp pulls in the full imgui API. Same-type
// redefinition is legal alongside <imgui.h>.
using ImGuiID = unsigned int;
#include <functional>
#include <utility>
#include <vector>

namespace edt
{
	// The full-window dock host: main menu bar and the default layout, both built from the registries.
	class dockspace
	{
	public:
		void render(panel_registry& panels, action_registry& actions);

		// Drawn inside the menu bar after the menus (live mode's status, #445); it draws nothing while it has nothing
		// to show, so the bar stays as it was. Null: nothing.
		void set_menu_bar_status(std::function<void()> draw) { m_menu_bar_status = std::move(draw); }

	private:
		void setup_default_layout(ImGuiID dockspace_id, const std::vector<panel_info>& panels);

		bool m_layout_initialized = false;
		std::function<void()> m_menu_bar_status;
	};
}
