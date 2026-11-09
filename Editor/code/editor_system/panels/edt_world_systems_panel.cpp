#include "edt_world_systems_panel.h"
#include <imgui.h>
#include <string>
#include <utility>

edt::world_systems_panel::world_systems_panel(editor_events& events, world_systems_model::ports ports)
	: panel_base("world_systems", "World Systems"), m_model(events, std::move(ports))
{
}

void edt::world_systems_panel::on_render()
{
	m_model.refresh_if_changed();
	const bool locked = m_model.locked();
	if (m_model.world().empty()) {
		ImGui::TextDisabled("No world is open");
		return;
	}
	ImGui::TextUnformatted(m_model.world().c_str());
	if (locked) {
		ImGui::SameLine();
		ImGui::TextDisabled("(locked while Play runs)");
	}
	if (!m_model.error().empty()) ImGui::TextColored({ 1.f, 0.3f, 0.3f, 1.f }, "%s", m_model.error().c_str());

	ImGui::BeginDisabled(locked || m_model.available().empty());
	if (ImGui::BeginCombo("##add_system", "Add system...")) {
		std::string chosen;
		for (const auto& candidate : m_model.available()) {
			const std::string label = candidate.name + "  [" + candidate.phase + "]";
			if (ImGui::Selectable(label.c_str())) chosen = candidate.name;
		}
		ImGui::EndCombo();
		if (!chosen.empty()) m_model.add(chosen);
	}
	ImGui::EndDisabled();
	ImGui::Separator();

	constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##world_systems_rows", 3, flags)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("System");
		ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 110.f);
		ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 120.f);
		ImGui::TableHeadersRow();
		const auto& systems = m_model.systems();
		std::string pending_remove;
		std::string pending_move;
		std::size_t pending_to = 0;
		for (std::size_t i = 0; i < systems.size(); ++i) {
			const auto& system = systems[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (system.known) {
				ImGui::TextUnformatted(system.name.c_str());
			} else {
				ImGui::TextColored({ 1.f, 0.8f, 0.f, 1.f }, "%s (unknown)", system.name.c_str());
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", system.known ? system.phase.c_str() : "-");
			ImGui::TableSetColumnIndex(2);
			ImGui::BeginDisabled(locked);
			ImGui::BeginDisabled(i == 0 || !system.known);
			if (ImGui::SmallButton("Up")) {
				pending_move = system.name;
				pending_to = i - 1;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(i + 1 == systems.size() || !system.known);
			if (ImGui::SmallButton("Down")) {
				pending_move = system.name;
				pending_to = i + 1;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove")) pending_remove = system.name;
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndTable();
		if (!pending_remove.empty()) {
			m_model.remove(pending_remove);
		} else if (!pending_move.empty()) {
			m_model.move(pending_move, pending_to);
		}
	}
}
