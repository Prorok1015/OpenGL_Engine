#include "edt_code_section.h"
#include <snk/common/logger/engine_log.h>
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"
#include <utility>

namespace edt
{
	namespace {

		const char* tag_of_category(prj::code_category category)
		{
			switch (category) {
			case prj::code_category::source: return "[C++]";
			case prj::code_category::header: return "[H]  ";
			case prj::code_category::cmake: return "[CMK]";
			default: return "[F]  ";
			}
		}

		constexpr const char* k_dialog_id = "Code file##edt_code_dialog";

	}

	code_section::code_section(res::resource_system& res, code_actions actions)
		: m_res(res), m_actions(std::move(actions))
	{
		m_res.subscribe_changes(prj::k_code_protocol, this, [this](std::span<const res::file_change>) { mark_changed(); });
	}

	code_section::~code_section()
	{
		m_res.unsubscribe_changes(this);
	}

	const std::vector<code_entry>& code_section::listing(const std::string& relative)
	{
		auto found = m_listings.find(relative);
		if (found == m_listings.end()) {
			found = m_listings.emplace(relative, m_actions.tree().list(relative)).first;
		}
		return found->second;
	}

	void code_section::report(const res::file_operation& result)
	{
		if (!result.ok) {
			m_error = result.error;
			egLOG_WARN("editor/code", "{}", result.error);
		}
	}

	void code_section::open_dialog(dialog_kind kind, std::string target, std::string initial_text)
	{
		m_dialog = kind;
		m_target = std::move(target);
		m_text = std::move(initial_text);
		m_error.clear();
		m_open_requested = true;
	}

	void code_section::draw_entry_menu(const code_entry& entry)
	{
		if (!ImGui::BeginPopupContextItem()) {
			return;
		}
		if (entry.is_directory) {
			if (ImGui::MenuItem("New file...")) {
				open_dialog(dialog_kind::new_file, entry.relative, "");
			}
		}
		else {
			if (ImGui::MenuItem("Open in editor")) {
				report(m_actions.open_in_ide(entry.relative));
			}
			if (ImGui::MenuItem("Rename...")) {
				open_dialog(dialog_kind::rename_file, entry.relative, entry.name);
			}
			if (ImGui::MenuItem("Delete...")) {
				open_dialog(dialog_kind::delete_file, entry.relative, "");
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Copy path")) {
				ImGui::SetClipboardText(m_actions.tree().physical_path(entry.relative).string().c_str());
			}
		}
		ImGui::EndPopup();
	}

	void code_section::draw_folder(const std::string& relative)
	{
		// A copy: a dialog or an event may clear the cache while the entries are drawn.
		const std::vector<code_entry> entries = listing(relative);
		for (const auto& entry : entries) {
			if (entry.is_directory) {
				const bool opened = ImGui::TreeNodeEx(entry.name.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth);
				draw_entry_menu(entry);
				if (opened) {
					draw_folder(entry.relative);
					ImGui::TreePop();
				}
				continue;
			}
			const std::string label = std::string{ tag_of_category(entry.category) } + " " + entry.name;
			ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				report(m_actions.open_in_ide(entry.relative));
			}
			draw_entry_menu(entry);
		}
		if (entries.empty() && relative.empty()) {
			ImGui::TextDisabled("(no code files)");
		}
	}

	void code_section::render()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12.f);
		const bool opened = ImGui::TreeNodeEx("code://##codetreeroot", ImGuiTreeNodeFlags_SpanFullWidth);
		if (ImGui::BeginPopupContextItem("##codetreeroot_menu")) {
			if (ImGui::MenuItem("New file...")) {
				open_dialog(dialog_kind::new_file, "", "");
			}
			ImGui::EndPopup();
		}
		if (opened) {
			draw_folder({});
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();
		if (!m_error.empty() && m_dialog == dialog_kind::none) {
			ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_error.c_str());
		}
		draw_dialogs();
	}

	void code_section::draw_dialogs()
	{
		if (m_open_requested) {
			ImGui::OpenPopup(k_dialog_id);
			m_open_requested = false;
		}
		if (!ImGui::BeginPopupModal(k_dialog_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}

		bool close = false;
		switch (m_dialog) {
		case dialog_kind::new_file:
			ImGui::Text("New file in code://%s", m_target.c_str());
			ImGui::SetNextItemWidth(320.f);
			ImGui::InputText("##name", &m_text);
			break;
		case dialog_kind::rename_file:
			ImGui::Text("Rename code://%s", m_target.c_str());
			ImGui::SetNextItemWidth(320.f);
			ImGui::InputText("##name", &m_text);
			break;
		case dialog_kind::delete_file:
			ImGui::Text("Delete code://%s?", m_target.c_str());
			ImGui::TextDisabled("The file is removed from the disk.");
			break;
		case dialog_kind::none:
			close = true;
			break;
		}
		if (!m_error.empty()) {
			ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_error.c_str());
		}

		const char* confirm = m_dialog == dialog_kind::delete_file ? "Delete" : (m_dialog == dialog_kind::rename_file ? "Rename" : "Create");
		if (ImGui::Button(confirm)) {
			res::file_operation result;
			if (m_dialog == dialog_kind::new_file) {
				result = m_actions.create_file(m_target, m_text);
			}
			else if (m_dialog == dialog_kind::rename_file) {
				result = m_actions.rename_file(m_target, m_text);
			}
			else {
				result = m_actions.delete_file(m_target);
			}
			if (result.ok) {
				m_error.clear();
				mark_changed();
				close = true;
			}
			else {
				report(result);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			m_error.clear();
			close = true;
		}

		if (close) {
			m_dialog = dialog_kind::none;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
