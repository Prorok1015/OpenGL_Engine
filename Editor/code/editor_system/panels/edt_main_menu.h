#pragma once
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_panel_registry.h>
#include <vector>

namespace edt
{
	// Declares the menus in the order the editor shows them (File, View, Build, Help) and the placeholder "About" item.
	// Keep the returned registrations as long as the menus should exist.
	std::vector<registration> register_builtin_menus(action_registry& actions);

	// Draws the items of the registries inside an open menu bar and fires the shortcuts. The View menu shows
	// its actions, then a toggle per panel. Shortcut keys: letters, digits, F1-F12, Delete, Escape, Enter, Space,
	// Tab, Backspace, Insert, Home, End, PageUp, PageDown and the arrows; any other key never fires.
	void draw_main_menu(action_registry& actions, panel_registry& panels);
}
