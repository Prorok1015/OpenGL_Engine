#include "edt_asset_export_dialog.h"
#include "res_system.h"
#include <imgui.h>

edt::asset_export_dialog::asset_export_dialog(asset_exporter& exporter)
	: m_exporter(exporter)
{
}

void edt::asset_export_dialog::open(import_result result)
{
	m_import = std::move(result);
	m_is_open = true;
	m_exported_tag = res::tag::null;

	std::string name = m_import.source_path.stem().string();
	strncpy(m_asset_name, name.c_str(), sizeof(m_asset_name) - 1);
	m_asset_name[sizeof(m_asset_name) - 1] = '\0';

	std::string folder = "assets/" + name;
	strncpy(m_target_folder, folder.c_str(), sizeof(m_target_folder) - 1);
	m_target_folder[sizeof(m_target_folder) - 1] = '\0';

	m_browse_folder = false;
	m_needs_open = true;
	m_folder_dialog.set_current_path(res::resource_system::get_resources_path());
	m_folder_dialog.set_select_mode(file_dialog::SELECT_MODE::FOLDERS_ONLY);
	m_folder_dialog.clear_extension_filters();
}

bool edt::asset_export_dialog::render()
{
	if (!m_is_open)
		return false;

	if (m_needs_open) {
		ImGui::OpenPopup("Export to Project##edt_export");
		m_needs_open = false;
	}

	// Handle folder browser sub-dialog
	if (m_browse_folder) {
		bool folder_open = true;
		if (m_folder_dialog.show("Select Target Folder", &folder_open)) {
			auto selected = m_folder_dialog.get_selected_path();
			auto base = res::resource_system::get_resources_path();
			auto rel = selected.lexically_relative(base);
			strncpy(m_target_folder, rel.string().c_str(), sizeof(m_target_folder) - 1);
			m_target_folder[sizeof(m_target_folder) - 1] = '\0';
			m_browse_folder = false;
		}
		if (!folder_open) {
			m_browse_folder = false;
		}
		return true;
	}

	bool is_open = true;

	if (ImGui::BeginPopupModal("Export to Project##edt_export", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Asset name:");
		ImGui::SetNextItemWidth(400.f);
		ImGui::InputText("##asset_name", m_asset_name, sizeof(m_asset_name));

		ImGui::Spacing();

		ImGui::Text("Target folder (relative to res://):");
		ImGui::SetNextItemWidth(330.f);
		ImGui::InputText("##target_folder", m_target_folder, sizeof(m_target_folder));
		ImGui::SameLine();
		if (ImGui::Button("Browse...")) {
			m_browse_folder = true;
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// Preview files
		std::string asset_name(m_asset_name);
		std::string target_folder(m_target_folder);

		if (!asset_name.empty() && !target_folder.empty()) {
			auto remap = m_exporter.build_remap(m_import, asset_name, target_folder);

			ImGui::Text("Files to be created:");
			ImGui::BeginChild("##file_list", ImVec2(450, 150), ImGuiChildFlags_Borders);
			for (const auto& [mem_tag, res_tag] : remap) {
				std::string rel(res_tag.relative());
				bool exists = std::filesystem::exists(
					res::resource_system::get_resources_path() / rel);
				if (exists)
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s [overwrite]", rel.c_str());
				else
					ImGui::Text("%s", rel.c_str());
			}
			ImGui::EndChild();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const float bw = 110.f;
		const float avail = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

		bool can_export = !asset_name.empty() && !target_folder.empty();

		if (!can_export) ImGui::BeginDisabled();
		if (ImGui::Button("Export", { bw, 0.f })) {
			auto remap = m_exporter.build_remap(m_import, asset_name, target_folder);
			if (m_exporter.export_to_project(m_import, remap)) {
				// Find the exported prefab tag
				if (auto it = remap.find(m_import.root_tag); it != remap.end())
					m_exported_tag = it->second;

				egLOG("edt/export", "Export successful: {}", m_exported_tag.view());
			}
			m_is_open = false;
			ImGui::CloseCurrentPopup();
		}
		if (!can_export) ImGui::EndDisabled();

		ImGui::SameLine();

		if (ImGui::Button("Cancel", { bw, 0.f })) {
			m_is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		// Popup was closed externally
		m_is_open = false;
		is_open = false;
	}

	return is_open;
}
