#include "edt_data_document_panel.h"
#include "edt_data_document.h"
#include "edt_data_document_view.h"
#include "edt_schema_inspector.h"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <boost/json/serialize.hpp>
#include <imgui.h>

namespace
{
	constexpr ImVec4 k_warning{ 1.0f, 0.8f, 0.3f, 1.0f };
	constexpr ImVec4 k_error{ 1.0f, 0.4f, 0.4f, 1.0f };
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

void edt::data_document_panel::on_hidden()
{
	// Collapsed or behind another tab the window is still open: only a closed one asks for its document to go. The editor
	// opens the window again when it keeps the document (unsaved changes), so this is not asked every frame.
	if (!is_open() && m_on_close) {
		m_on_close();
	}
}

void edt::data_document_panel::on_render()
{
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
			const schema_ui::context context{ descs->schemas(),
				[](const std::string& label, boost::json::value& value) {
					ImGui::TextDisabled("%s: %s", label.c_str(), boost::json::serialize(value).c_str());
					return false;
				},
				{} };
			m_document.edit([&](boost::json::value& shown) { return schema_ui::draw_fields(*schema, shown, context); });
		}
	}
	ImGui::EndChild();
}
