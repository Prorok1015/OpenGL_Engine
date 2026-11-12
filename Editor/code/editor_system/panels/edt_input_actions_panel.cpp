#include "edt_input_actions_panel.h"
#include <snk/gui/gui_platform_backend.h>
#include <imgui.h>

namespace edt
{
	input_actions_panel::input_actions_panel()
		: panel_base("input_actions", "Input Actions")
	{
	}

	void input_actions_panel::rebuild_rows(const gs::input_actions& actions)
	{
		m_rows = input_actions_view::build_rows(*actions.project_map(), *actions.map(), actions.conflicts());
		m_rows_revision = actions.revision();
	}

	void input_actions_panel::update_capture(gs::input_actions& actions)
	{
		if (!m_capture.active()) return;
		// The window is the only place a key is taken from: another window, or another application, ends the wait.
		if (ImGui::GetIO().AppFocusLost || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
			m_capture.cancel();
			return;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			m_capture.cancel();
			return;
		}

		std::optional<inp::action_binding> picked;
		if (const auto key = gui::pressed_key()) {
			picked = *key;
		} else if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
			if (const auto button = gui::clicked_mouse_button()) picked = *button;
		}
		if (!picked && m_gamepad) picked = m_gamepad();
		if (!picked) return;

		const std::string action = m_capture.action();
		const std::size_t slot = m_capture.slot();
		m_capture.cancel();
		m_message = input_actions_view::outcome_text(actions.rebind(action, slot, *picked, true), action, &*picked);
	}

	void input_actions_panel::on_render()
	{
		gs::input_actions* actions = m_actions ? m_actions() : nullptr;
		if (!actions || !actions->map() || !actions->project_map()) {
			m_capture.cancel();
			ImGui::TextDisabled("The project has no input actions");
			return;
		}
		update_capture(*actions);
		if (m_rows_revision != actions->revision()) rebuild_rows(*actions);

		// What a button asked for; done after the table, which reads rows a change replaces.
		struct request
		{
			enum class kind { none, assign, remove, reset, reset_all } what = kind::none;
			std::string action;
			std::size_t slot = 0;
		} asked;

		if (ImGui::Button("Reset all")) asked.what = request::kind::reset_all;
		ImGui::SameLine();
		if (m_capture.active()) {
			ImGui::TextUnformatted("Press a key or a mouse button; Esc cancels");
		} else if (!m_message.empty()) {
			ImGui::TextUnformatted(m_message.c_str());
		} else {
			ImGui::TextDisabled("Click a binding to change it");
		}
		if (!actions->user_file_saved()) ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "The player's bindings could not be saved");
		ImGui::Separator();

		constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##input_actions_rows", 3, flags)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.f);
			ImGui::TableSetupColumn("Bindings");
			ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 60.f);
			ImGui::TableHeadersRow();
			for (const input_actions_view::action_row& row : m_rows) {
				ImGui::PushID(row.name.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(row.name.c_str());
				if (!row.shares_with.empty()) {
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "!");
					if (ImGui::IsItemHovered()) {
						std::string others;
						for (const std::string& other : row.shares_with) others += (others.empty() ? "" : ", ") + other;
						ImGui::SetTooltip("Shares a binding with: %s", others.c_str());
					}
				}
				ImGui::TableSetColumnIndex(1);
				for (std::size_t slot = 0; slot < row.bindings.size(); ++slot) {
					ImGui::PushID(static_cast<int>(slot));
					const bool waiting = m_capture.waits_on(row.name, slot);
					const std::string label = waiting ? std::string{ "press..." } : row.bindings[slot].text + (row.bindings[slot].changed ? " *" : "");
					if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
					if (ImGui::Button(label.c_str())) asked = { request::kind::assign, row.name, slot };
					if (waiting) ImGui::PopStyleColor();
					ImGui::SameLine(0.f, 2.f);
					if (ImGui::SmallButton("x")) asked = { request::kind::remove, row.name, slot };
					ImGui::SameLine();
					ImGui::PopID();
				}
				if (ImGui::SmallButton("+")) asked = { request::kind::assign, row.name, row.bindings.size() };
				ImGui::TableSetColumnIndex(2);
				ImGui::BeginDisabled(!row.changed);
				if (ImGui::Button("Reset")) asked = { request::kind::reset, row.name, 0 };
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		switch (asked.what) {
		case request::kind::none:
			break;
		case request::kind::assign:
			m_message.clear();
			m_capture.begin(asked.action, asked.slot);
			break;
		case request::kind::remove:
			m_capture.cancel();
			m_message = input_actions_view::outcome_text(actions->remove_binding(asked.action, asked.slot), asked.action, nullptr);
			break;
		case request::kind::reset:
			m_capture.cancel();
			m_message.clear();
			actions->reset_binding(asked.action);
			break;
		case request::kind::reset_all:
			m_capture.cancel();
			m_message.clear();
			actions->reset_all_bindings();
			break;
		}
	}
}
