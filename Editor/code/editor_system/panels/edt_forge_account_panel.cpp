#include "edt_forge_account_panel.h"
#include <imgui.h>
#include <string>
#include <utility>

namespace {
	const ImVec4 k_error{ 1.0f, 0.45f, 0.40f, 1.0f };
	const ImVec4 k_ok{ 0.45f, 0.80f, 0.45f, 1.0f };
}

edt::forge_account_panel::forge_account_panel(editor_events& events, std::vector<std::string> hosts, forge_account_model::ports ports)
	: panel_base(k_id, k_title), m_model(events, std::move(hosts), std::move(ports))
{
	m_open = false;
	m_buffers.resize(m_model.rows().size());
	wipe_all();
}

edt::forge_account_panel::~forge_account_panel()
{
	wipe_all();
}

void edt::forge_account_panel::wipe_all()
{
	for (buffer& entry : m_buffers) wipe_buffer(entry.data(), entry.size());
}

void edt::forge_account_panel::on_before_begin()
{
	ImGui::SetNextWindowSize(ImVec2(560.f, 220.f), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::forge_account_panel::on_hidden()
{
	wipe_all();
	m_model.set_visible(false);
}

void edt::forge_account_panel::on_render()
{
	m_model.set_visible(true);
	ImGui::TextWrapped("The token is kept in the OS keychain when there is one, else for this session only. It is never shown, "
		"and neither the project nor an agent can read or set it.");
	ImGui::Separator();

	const std::vector<forge_account_model::row>& rows = m_model.rows();
	if (rows.empty()) ImGui::TextDisabled("No code host is allowed.");
	for (std::size_t i = 0; i < rows.size() && i < m_buffers.size(); ++i) {
		const forge_account_model::row& row = rows[i];
		buffer& field = m_buffers[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::TextUnformatted(row.host.c_str());
		ImGui::SameLine();
		const std::string_view status = forge_token_state_text(row.state);
		ImGui::TextDisabled("%s", std::string(status).c_str());

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 11.0f);
		ImGui::BeginDisabled(row.busy);
		bool save = ImGui::InputTextWithHint("##token", "paste a token", field.data(), field.size(),
			ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		ImGui::BeginDisabled(field[0] == '\0');
		save = ImGui::Button("Save") || save;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(row.state == forge_token_state::not_set || row.state == forge_token_state::checking);
		const bool remove = ImGui::Button("Remove");
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		// Save wipes the field before its job starts, a refusal included: the person types it again.
		if (save && field[0] != '\0') m_model.save_from_buffer(row.host, field.data(), field.size());
		if (remove) m_model.remove(row.host);
		if (row.busy) {
			ImGui::SameLine();
			ImGui::TextDisabled("working...");
		}
		ImGui::PopID();
	}

	if (!m_model.message().empty()) {
		ImGui::Separator();
		ImGui::PushStyleColor(ImGuiCol_Text, m_model.message_is_error() ? k_error : k_ok);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(m_model.message().c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::SmallButton("Dismiss")) m_model.clear_message();
	}
}
