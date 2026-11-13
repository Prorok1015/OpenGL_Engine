#include "edt_data_document_panel.h"
#include "edt_data_document.h"
#include "edt_data_document_view.h"
#include "edt_schema_inspector.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <boost/json/serialize.hpp>
#include <imgui.h>
#include <imgui_internal.h>

namespace
{
	constexpr ImVec4 k_warning{ 1.0f, 0.8f, 0.3f, 1.0f };
	constexpr ImVec4 k_error{ 1.0f, 0.4f, 0.4f, 1.0f };

	// The main menu bar lives in a window of the dockspace host: a click on a menu moves the focus there, and closing the
	// menu leaves it there. None of these is a window the person works in, so the document they came from stays active.
	std::string_view window_taking_focus()
	{
		const ImGuiWindow* taken = ImGui::GetCurrentContext()->NavWindow;
		if (taken == nullptr) return {};
		taken = taken->RootWindow;
		constexpr ImGuiWindowFlags service = ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip
			| ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal;
		if ((taken->Flags & service) != 0 || (taken->Name[0] == '#' && taken->Name[1] == '#')) return {};
		return taken->Name;
	}
}

edt::data_document_panel::data_document_panel(data_document& document, editor_services& services, document_id id)
	: panel_base(data_document_panel_id(id), data_document_window_title(document, id))
	, m_document(document)
	, m_services(services)
	, m_id(id)
{
}

void edt::data_document_panel::on_before_begin()
{
	ImGui::SetNextWindowSize(ImVec2(520.f, 460.f), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::data_document_panel::follow_focus()
{
	// A menu or a dialog over the window takes the focus from it for a moment without the person leaving the document.
	if (ImGui::IsPopupOpen(static_cast<const char*>(nullptr), ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
		return;
	}
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (focused == m_focused) {
		return;
	}
	if (focused) {
		m_focused = true;
		if (m_on_focus) m_on_focus();
	} else {
		lose_focus();
	}
}

// An edit in the middle of a drag is recorded before another window takes over: it must not be folded into an edit made there.
void edt::data_document_panel::lose_focus()
{
	m_focused = false;
	m_document.commit_pending();
	if (m_on_blur) m_on_blur(window_taking_focus());
}

void edt::data_document_panel::on_hidden()
{
	if (m_focused) {
		lose_focus();
	}
	// Collapsed or behind another tab the window is still open: only a closed one asks for its document to go. The editor
	// opens the window again when it keeps the document (unsaved changes), so this is not asked every frame.
	if (!is_open() && m_on_close) {
		m_on_close();
	}
}

void edt::data_document_panel::on_render()
{
	follow_focus();
	const data_document_status status = status_of(m_document);
	ImGui::TextDisabled("%s", status.path.c_str());
	if (!status.parents.empty()) {
		ImGui::TextDisabled("inherits: %s", status.parents.c_str());
	}
	if (!status.chain_problem.empty()) {
		ImGui::TextColored(k_warning, "A parent could not be read, the last good values are shown: %s", status.chain_problem.c_str());
	}
	if (!status.changed_on_disk) {
		m_dismissed = false;
	} else if (!m_dismissed || m_dismissed_serial != m_document.disk_change_serial()) {
		ImGui::TextColored(k_warning, "The file changed on disk since it was opened.");
		if (m_on_reload && m_on_overwrite) {
			ImGui::SameLine();
			if (ImGui::SmallButton("Reload")) m_on_reload();
			ImGui::SameLine();
			if (ImGui::SmallButton("Overwrite")) m_on_overwrite();
			ImGui::SameLine();
			if (ImGui::SmallButton("Later")) {
				m_dismissed = true;
				m_dismissed_serial = m_document.disk_change_serial();
			}
		}
	}

	ImGui::BeginDisabled(!status.dirty || !status.can_save);
	if (ImGui::Button("Save")) {
		if (m_on_save) {
			m_on_save();
		} else {
			const save_result saved = m_document.save();
			m_error = saved.saved ? std::string{} : saved.error;
			if (!saved.saved) egLOG_WARN("editor", "{}", saved.error);
		}
	}
	ImGui::EndDisabled();
	if (status.dirty) {
		ImGui::SameLine();
		ImGui::TextDisabled("unsaved changes");
	}
	if (!m_error.empty()) {
		ImGui::TextColored(k_error, "%s", m_error.c_str());
	}
	ImGui::Separator();

	if (ImGui::BeginChild("##fields", ImVec2(0.f, 0.f), ImGuiChildFlags_None)) {
		const desc::type_schema* schema = m_document.schema();
		const desc::desc_system* descs = m_services.descs();
		if (!schema || !descs) {
			ImGui::TextDisabled("The type of this document is not registered any more.");
		} else {
			// Keys typed or refused for the content shown before are of no use to the content shown now; the window's own
			// edits move the generation too, and are taken in below.
			if (m_document.generation() != m_state_generation) {
				m_map_state.clear();
				m_state_generation = m_document.generation();
			}
			schema_ui::context context{ descs->schemas(),
				[](const std::string& label, boost::json::value& value) {
					ImGui::TextDisabled("%s: %s", label.c_str(), boost::json::serialize(value).c_str());
					return false;
				},
				{} };
			context.map_state = &m_map_state;
			m_document.edit([&](boost::json::value& shown) { return schema_ui::draw_fields(*schema, shown, context); });
			m_state_generation = m_document.generation();
		}
	}
	ImGui::EndChild();
	// One record for what a drag or a typed value did, taken once the widget is let go.
	if (!ImGui::IsAnyItemActive()) {
		m_document.commit_pending();
	}
}
