#include "edt_asset_browser_panel.h"
#include "edt_code_section.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>

namespace edt
{
	// ─── helpers ──────────────────────────────────────────────────────────────

	static std::string to_lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	const char* asset_browser_panel::get_file_icon(const std::filesystem::path& p) const
	{
		std::string ext = to_lower(p.extension().string());
		if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx") return "[3D] ";
		if (ext == ".desc")                                                      return "[D]  ";
		if (ext == ".png"  || ext == ".jpg"  || ext == ".jpeg" ||
		    ext == ".tga"  || ext == ".hdr"  || ext == ".bmp")                  return "[IMG]";
		if (ext == ".vert" || ext == ".frag" || ext == ".geom" || ext == ".glsl") return "[SHD]";
		if (ext == ".wav"  || ext == ".ogg"  || ext == ".mp3")                  return "[SND]";
		return "[F]  ";
	}

	// ─── lifecycle ────────────────────────────────────────────────────────────

	asset_browser_panel::asset_browser_panel()
		: panel_base("asset_browser", "Asset Browser")
	{
	}

	void asset_browser_panel::set_layers(std::vector<std::filesystem::path> layers)
	{
		m_layers = std::move(layers);
		m_current_rel.clear();
		m_needs_refresh = true;
	}

	void asset_browser_panel::set_code_section(std::shared_ptr<code_section> section)
	{
		m_code_section = std::move(section);
	}

	void asset_browser_panel::navigate_to(const std::filesystem::path& rel_dir)
	{
		m_current_rel   = rel_dir;
		m_needs_refresh = true;
	}

	// ─── refresh ──────────────────────────────────────────────────────────────

	void asset_browser_panel::refresh_entries()
	{
		m_entries.clear();
		m_needs_refresh = false;

		const std::string search_lower = to_lower(m_search);

		for (auto& src : list_merged_directory(m_layers, m_current_rel)) {
			// Search filter (dirs always pass)
			if (!search_lower.empty() && !src.is_dir) {
				if (to_lower(src.name).find(search_lower) == std::string::npos)
					continue;
			}
			std::string label = src.is_dir
				? "[/] " + src.name
				: std::string(get_file_icon(src.path)) + " " + src.name;
			m_entries.push_back({std::move(src), std::move(label)});
		}
	}

	// ─── left panel: directory tree ───────────────────────────────────────────

	void asset_browser_panel::draw_dir_tree(const std::filesystem::path& rel_dir, int depth)
	{
		if (depth > 6)
			return;

		for (const auto& sub : list_merged_directory(m_layers, rel_dir, true)) {
			const bool selected  = (sub.relative == m_current_rel);
			// Auto-expand if m_current_rel is inside this subtree
			const bool is_ancestor = [&] {
				auto p = m_current_rel;
				while (p != p.parent_path()) {
					if (p == sub.relative) return true;
					p = p.parent_path();
				}
				return false;
			}();

			ImGuiTreeNodeFlags flags =
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_SpanFullWidth;
			if (selected)   flags |= ImGuiTreeNodeFlags_Selected;
			if (is_ancestor) ImGui::SetNextItemOpen(true, ImGuiCond_Once);

			const bool opened = ImGui::TreeNodeEx(sub.name.c_str(), flags);

			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				navigate_to(sub.relative);

			if (opened) {
				draw_dir_tree(sub.relative, depth + 1);
				ImGui::TreePop();
			}
		}
	}

	// ─── render ───────────────────────────────────────────────────────────────

	void asset_browser_panel::on_render()
	{
		if (m_layers.empty()) {
			ImGui::TextDisabled("(base path not set)");
			return;
		}

		// ── breadcrumb ───────────────────────────────────────────────────────
		{
			// Root button
			if (ImGui::SmallButton("res://"))
				navigate_to({});

			// One button per path segment between res:// and current
			std::filesystem::path cursor;
			for (const auto& part : m_current_rel) {
				if (part == ".") continue;
				cursor /= part;
				ImGui::SameLine(0.f, 2.f);
				ImGui::TextDisabled(">");
				ImGui::SameLine(0.f, 2.f);
				const std::string seg = part.string();
				if (ImGui::SmallButton(seg.c_str()))
					navigate_to(cursor);
			}
		}

		// ── search ───────────────────────────────────────────────────────────
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::InputTextWithHint("##search", "Search files...", m_search, sizeof(m_search)))
			m_needs_refresh = true;

		ImGui::Separator();

		// ── two columns: tree | files ─────────────────────────────────────────
		ImGui::Columns(2, "ab_cols", true);
		ImGui::SetColumnWidth(0, 160.f);

		// Left: directory tree
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12.f);
		if (ImGui::TreeNodeEx("res://##directorytreeroot", ImGuiTreeNodeFlags_DefaultOpen |
		                                ImGuiTreeNodeFlags_SpanFullWidth))
		{
			draw_dir_tree({});
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();

		if (m_code_section)
			m_code_section->render();

		ImGui::NextColumn();

		// Right: current-directory contents
		if (m_needs_refresh)
			refresh_entries();

		// ".." back button (disabled at root)
		{
			bool at_root = m_current_rel.empty();
			if (at_root) ImGui::BeginDisabled();
			if (ImGui::Selectable("[/] .."))
				navigate_to(m_current_rel.parent_path());
			if (at_root) ImGui::EndDisabled();
		}

		const size_t top_layer = m_layers.size() - 1;
		for (const auto& e : m_entries) {
			// Entries that come from a lower layer are dimmed; the tooltip names the layer root.
			const bool from_lower_layer = e.src.layer != top_layer;
			if (from_lower_layer)
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::Selectable(e.label.c_str());
			if (from_lower_layer)
				ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && m_layers.size() > 1)
				ImGui::SetTooltip("layer %zu: %s", e.src.layer, m_layers[e.src.layer].string().c_str());

			if (e.src.is_dir) {
				// Navigate into folder on click
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					navigate_to(e.src.relative);
			} else {
				// Double-click on file
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (m_on_file_double_clicked)
						m_on_file_double_clicked(e.src.relative);
				}

				// Drag source: path relative to res:// (the physical path would be wrong for lower layers)
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
					const std::string full = e.src.relative.generic_string();
					ImGui::SetDragDropPayload("ASSET_PATH", full.c_str(), full.size() + 1);
					ImGui::TextUnformatted(e.label.c_str());
					ImGui::EndDragDropSource();
				}

				// Context menu
				if (ImGui::BeginPopupContextItem()) {
					if (ImGui::MenuItem("Copy path"))
						ImGui::SetClipboardText(e.src.path.string().c_str());
					if (ImGui::MenuItem("Copy relative path")) {
						ImGui::SetClipboardText(e.src.relative.generic_string().c_str());
					}
					ImGui::EndPopup();
				}
			}
		}

		if (m_entries.empty())
			ImGui::TextDisabled("(empty)");

		ImGui::Columns(1);
	}
}
