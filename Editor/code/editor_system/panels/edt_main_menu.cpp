#include "edt_main_menu.h"
#include <imgui.h>
#include <optional>
#include <string_view>

namespace {
	ImGuiKey key_from_name(std::string_view name)
	{
		if (name.size() == 1) {
			const char c = name[0];
			if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
			if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
			switch (c) {
			case '`': return ImGuiKey_GraveAccent;
			case '[': return ImGuiKey_LeftBracket;
			case ']': return ImGuiKey_RightBracket;
			case '\\': return ImGuiKey_Backslash;
			case '\'': return ImGuiKey_Apostrophe;
			case ',': return ImGuiKey_Comma;
			case '-': return ImGuiKey_Minus;
			case '.': return ImGuiKey_Period;
			case '/': return ImGuiKey_Slash;
			case ';': return ImGuiKey_Semicolon;
			case '=': return ImGuiKey_Equal;
			default: return ImGuiKey_None;
			}
		}
		if (name.size() <= 3 && name[0] == 'F') {
			int number = 0;
			for (const char c : name.substr(1)) {
				if (c < '0' || c > '9') return ImGuiKey_None;
				number = number * 10 + (c - '0');
			}
			if (number >= 1 && number <= 12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (number - 1));
			return ImGuiKey_None;
		}
		struct named { std::string_view name; ImGuiKey key; };
		static constexpr named NAMED[] = {
			{ "DELETE", ImGuiKey_Delete }, { "ESCAPE", ImGuiKey_Escape }, { "ENTER", ImGuiKey_Enter },
			{ "SPACE", ImGuiKey_Space }, { "TAB", ImGuiKey_Tab }, { "BACKSPACE", ImGuiKey_Backspace },
			{ "INSERT", ImGuiKey_Insert }, { "HOME", ImGuiKey_Home }, { "END", ImGuiKey_End },
			{ "PAGEUP", ImGuiKey_PageUp }, { "PAGEDOWN", ImGuiKey_PageDown },
			{ "UP", ImGuiKey_UpArrow }, { "DOWN", ImGuiKey_DownArrow },
			{ "LEFT", ImGuiKey_LeftArrow }, { "RIGHT", ImGuiKey_RightArrow },
		};
		for (const named& n : NAMED) {
			if (n.name == name) return n.key;
		}
		return ImGuiKey_None;
	}

	// Works regardless of menu focus.
	void fire_shortcuts(edt::action_registry& actions)
	{
		const ImGuiIO& io = ImGui::GetIO();
		std::optional<edt::shortcut> pressed;
		actions.for_each_shortcut([&](const edt::shortcut& chord) {
			if (pressed || chord.ctrl != io.KeyCtrl || chord.shift != io.KeyShift || chord.alt != io.KeyAlt) return;
			const ImGuiKey key = key_from_name(chord.key);
			if (key != ImGuiKey_None && ImGui::IsKeyPressed(key, false)) pressed = chord;
		});
		if (pressed) actions.dispatch(*pressed);
	}

	void draw_panel_toggles(edt::panel_registry& panels)
	{
		for (const edt::panel_info& panel : panels.panels()) {
			if (!panel.show_in_view_menu) continue;
			bool open = panel.open;
			if (ImGui::MenuItem(panel.title.c_str(), nullptr, &open))
				panels.set_open(panel.id, open);
		}
	}
}

std::vector<edt::registration> edt::register_builtin_menus(action_registry& actions)
{
	std::vector<registration> out;
	out.push_back(actions.add_menu("File"));
	out.push_back(actions.add_menu(std::string(view_menu_name)));
	out.push_back(actions.add_menu("Build"));
	out.push_back(actions.add_menu("Help"));
	out.push_back(actions.add({ .id = "help.about", .label = "About", .menu_path = "Help", .execute = no_args([] {}), .expose_as_tool = false }));
	return out;
}

void edt::draw_main_menu(action_registry& actions, panel_registry& panels)
{
	fire_shortcuts(actions);

	for (const std::string& menu : actions.menus()) {
		if (!ImGui::BeginMenu(menu.c_str())) continue;

		for (const menu_item_info& item : actions.items(menu)) {
			if (item.separator_before) ImGui::Separator();
			const char* shortcut_text = item.shortcut_text.empty() ? nullptr : item.shortcut_text.c_str();
			if (ImGui::MenuItem(item.label.c_str(), shortcut_text, false, item.enabled))
				actions.activate(item.id);
			if (!item.enabled && !item.disabled_tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", item.disabled_tooltip.c_str());
		}
		if (menu == view_menu_name) draw_panel_toggles(panels);
		ImGui::EndMenu();
	}
}
