#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_panel_registry.h>
#include <functional>
#include <string_view>
#include <vector>

namespace edt
{
	class ui_probe;

	// Declares the menus in the order the editor shows them (File, View, Build, Help) and the placeholder "About" item.
	// Keep the returned registrations as long as the menus should exist.
	std::vector<registration> register_builtin_menus(action_registry& actions);

	// Draws the items of the registries inside an open menu bar. The View menu shows its actions, then a toggle per panel.
	// With `probe` (headless smoke, editor/ui_probe) every menu and every item of an open menu is reported in the area
	// "menu".
	void draw_main_menu(action_registry& actions, panel_registry& panels, ui_probe* probe = nullptr);

	// Activates the action whose shortcut was pressed this ImGui frame (at most one; while ImGui wants text input, not
	// one that yields to it). Called once per UI frame by editor_layer before either layout draws, so the shortcuts work
	// without a menu bar (game mode). Keys: letters, digits, F1-F12, Delete, Escape, Enter, Space, Tab, Backspace,
	// Insert, Home, End, PageUp, PageDown and the arrows; any other key never fires. With `allowed`, only actions whose
	// id it accepts (game mode). True if an action ran.
	bool fire_shortcuts(action_registry& actions, const std::function<bool(std::string_view)>& allowed = {});
}
