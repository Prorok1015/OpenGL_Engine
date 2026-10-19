#include "edt_blackboard_panel.h"
#include "edt_blackboard_view.h"
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <imgui.h>

namespace edt
{
	blackboard_panel::blackboard_panel()
		: panel_base("blackboard", "Blackboard")
	{
	}

	void blackboard_panel::on_render()
	{
		const auto manager = m_manager ? m_manager() : nullptr;
		if (!manager) {
			ImGui::TextDisabled("No level is loaded");
			return;
		}

		const auto names = blackboard_view::set_names(*manager);
		m_set = blackboard_view::pick_set(m_set, names);
		scn::context_set* set = manager->find_set(m_set);
		if (!set) {
			ImGui::TextDisabled("No context sets");
			return;
		}

		ImGui::SetNextItemWidth(160.f);
		if (ImGui::BeginCombo("Set", m_set.c_str())) {
			for (const auto& name : names) {
				if (ImGui::Selectable(name.c_str(), name == m_set)) m_set = name;
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##blackboard_filter", "Filter keys", m_filter.data(), m_filter.size());

		const auto& board = set->channel().blackboard();
		ImGui::TextDisabled("%zu values, %zu written this frame, %llu exchanges%s",
			board.committed().size(), board.pending().size(),
			static_cast<unsigned long long>(set->channel().exchange_count()),
			set->active() ? "" : ", set is inactive");
		ImGui::Separator();

		const auto rows = blackboard_view::collect_rows(*set, m_filter.data());
		constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable
			| ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##blackboard_rows", 4, flags)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Key");
			ImGui::TableSetupColumn("Value");
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.f);
			ImGui::TableSetupColumn("Writer", ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableHeadersRow();
			for (const auto& row : rows) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(row.key.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(row.value.c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(row.type.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(row.writer.c_str());
			}
			ImGui::EndTable();
		}
	}
}
