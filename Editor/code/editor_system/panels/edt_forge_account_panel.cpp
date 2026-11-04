#include "edt_forge_account_panel.h"
#include <imgui.h>
#include <imgui_internal.h>
// imgui_internal.h only forward-declares ImStb::STB_TexteditState; the undo stack is read and wiped here, as imgui_widgets.cpp includes it.
namespace ImStb
{
#include <imstb_textedit.h>
}
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

namespace {
	const ImVec4 k_error{ 1.0f, 0.45f, 0.40f, 1.0f };
	const ImVec4 k_ok{ 0.45f, 0.80f, 0.45f, 1.0f };

	void wipe_vector(ImVector<char>& vector)
	{
		if (vector.Data != nullptr && vector.Capacity > 0) edt::wipe_buffer(vector.Data, static_cast<std::size_t>(vector.Capacity));
		vector.resize(0);
	}

	// The text-edit state is a member in some ImGui versions and a pointer in others.
	template <class state>
	auto* edit_state_of(state& input)
	{
		if constexpr (std::is_pointer_v<std::remove_reference_t<decltype(input.Stb)>>) return input.Stb;
		else return &input.Stb;
	}
}

void edt::wipe_imgui_token_state(unsigned int field_id)
{
	ImGuiContext* context = ImGui::GetCurrentContext();
	if (context == nullptr || field_id == 0) return;
	ImGuiContext& g = *context;
	if (g.InputTextState.ID != field_id && g.InputTextDeactivatedState.ID != field_id) return;

	// First: dropping the active item may make ImGui copy the text into the deactivated state, which is wiped below.
	if (g.ActiveId == field_id) ImGui::ClearActiveID();
	wipe_vector(g.InputTextState.TextA);
	wipe_vector(g.InputTextState.TextToRevertTo);
	if (auto* edit = edit_state_of(g.InputTextState)) {
		auto& undo = edit->undostate;
		wipe_buffer(reinterpret_cast<char*>(&undo), sizeof(undo));
		// The empty stack of stb_textedit: undo records grow up from 0, redo records down from the end.
		undo.redo_point = static_cast<decltype(undo.redo_point)>(sizeof(undo.undo_rec) / sizeof(undo.undo_rec[0]));
		undo.redo_char_point = static_cast<decltype(undo.redo_char_point)>(sizeof(undo.undo_char) / sizeof(undo.undo_char[0]));
	}
	wipe_vector(g.InputTextDeactivatedState.TextA);
	g.InputTextState.ID = 0;
	g.InputTextDeactivatedState.ID = 0;
}

edt::forge_account_panel::forge_account_panel(editor_events& events, std::vector<std::string> hosts, forge_account_model::ports ports)
	: panel_base(k_id, k_title), m_model(events, std::move(hosts), std::move(ports))
{
	m_open = false;
	m_buffers.resize(m_model.rows().size());
	m_field_ids.assign(m_buffers.size(), 0u);
	wipe_all();
}

edt::forge_account_panel::~forge_account_panel()
{
	wipe_all();
}

void edt::forge_account_panel::wipe_field(std::size_t index)
{
	wipe_buffer(m_buffers[index].data(), m_buffers[index].size());
	wipe_imgui_token_state(m_field_ids[index]);
}

void edt::forge_account_panel::wipe_all()
{
	for (std::size_t i = 0; i < m_buffers.size(); ++i) wipe_field(i);
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
	ImGui::BeginDisabled(m_model.busy());
	if (ImGui::SmallButton("Refresh")) m_model.refresh();
	ImGui::EndDisabled();
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

		m_field_ids[i] = ImGui::GetID("##token");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 15.0f);
		ImGui::BeginDisabled(row.busy);
		bool save = ImGui::InputTextWithHint("##token", "paste a token", field.data(), field.size(),
			ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		ImGui::BeginDisabled(field[0] == '\0');
		save = ImGui::Button("Save") || save;
		ImGui::EndDisabled();
		ImGui::SameLine();
		const bool clear = ImGui::Button("Clear");
		ImGui::SameLine();
		ImGui::BeginDisabled(row.state == forge_token_state::not_set || row.state == forge_token_state::checking);
		const bool remove = ImGui::Button("Remove");
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		// Save wipes the field before its job starts, a refusal included: the person types it again.
		if (save && field[0] != '\0') m_model.save_from_buffer(row.host, field.data(), field.size());
		if (remove) m_model.remove(row.host);
		// After the buttons: a click on one deactivated the field this frame, which is when ImGui copies its text.
		if (save || clear || remove) wipe_field(i);
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
