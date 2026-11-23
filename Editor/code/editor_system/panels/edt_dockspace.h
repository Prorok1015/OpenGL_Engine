#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_panel_registry.h>
// ImGuiID is an imgui integer typedef (unsigned int). Re-declared here to keep <imgui.h>
// out of this header (EPIC-49 US-49-4); the .cpp pulls in the full imgui API. Same-type
// redefinition is legal alongside <imgui.h>.
using ImGuiID = unsigned int;
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace edt
{
	class ui_probe;

	// The full-window dock host: main menu bar and the default layout, both built from the registries.
	class dockspace
	{
	public:
		void render(panel_registry& panels, action_registry& actions);
		// Instead of render() while another layout fills the window (game mode): the dock nodes stay alive, so the
		// docked windows are docked where they were when render() comes back. Nothing before the first render().
		void keep_alive();

		// Drawn inside the menu bar after the menus (live mode's status, #445); it draws nothing while it has nothing
		// to show, so the bar stays as it was. Null: nothing.
		void set_menu_bar_status(std::function<void()> draw) { m_menu_bar_status = std::move(draw); }
		// Drawn in the dock host between the menu bar and the dock space (the strip of document tabs, #1184): outside the dock
		// tree, so layout.ini stays valid. Not drawn in game mode (keep_alive replaces render()). Null: nothing.
		void set_tabs_bar(std::function<void()> draw) { m_tabs_bar = std::move(draw); }
		// Reports the menus and their items (headless smoke, editor/ui_probe); null: nothing.
		void set_ui_probe(std::shared_ptr<ui_probe> probe) { m_ui_probe = std::move(probe); }

	private:
		void setup_default_layout(ImGuiID dockspace_id, const std::vector<panel_info>& panels);

		bool m_layout_initialized = false;
		// Its id depends on the host window's ID stack: kept from render() for keep_alive().
		ImGuiID m_dockspace_id = 0;
		std::function<void()> m_menu_bar_status;
		std::function<void()> m_tabs_bar;
		std::shared_ptr<ui_probe> m_ui_probe;
	};
}
