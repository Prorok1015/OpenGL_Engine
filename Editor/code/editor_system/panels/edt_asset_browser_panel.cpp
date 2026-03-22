#include "edt_asset_browser_panel.h"
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

	void asset_browser_panel::set_base_path(std::filesystem::path base_path)
	{
		m_base_path  = std::move(base_path);
		m_current_dir = m_base_path;
		m_needs_refresh = true;
	}

	void asset_browser_panel::navigate_to(const std::filesystem::path& dir)
	{
		m_current_dir   = dir;
		m_needs_refresh = true;
	}

	// ─── refresh ──────────────────────────────────────────────────────────────

	void asset_browser_panel::refresh_entries()
	{
		m_entries.clear();
		m_needs_refresh = false;

		if (m_current_dir.empty())
			return;

		std::error_code ec;
		if (!std::filesystem::is_directory(m_current_dir, ec) || ec)
			return;

		const std::string search_lower = to_lower(m_search);

		std::vector<entry> dirs, files;
		for (auto it = std::filesystem::directory_iterator(m_current_dir, ec);
		     !ec && it != std::filesystem::directory_iterator{};
		     it.increment(ec))
		{
			std::error_code ec2;
			const bool is_dir = it->is_directory(ec2);
			if (ec2) continue;

			const std::filesystem::path& p = it->path();
			std::string name = p.filename().string();

			// Search filter (dirs always pass)
			if (!search_lower.empty() && !is_dir) {
				if (to_lower(name).find(search_lower) == std::string::npos)
					continue;
			}

			if (is_dir) {
				dirs.push_back({p, true, "[/] " + name});
			} else {
				files.push_back({p, false, std::string(get_file_icon(p)) + " " + name});
			}
		}

		auto by_path = [](const entry& a, const entry& b) { return a.path < b.path; };
		std::sort(dirs.begin(),  dirs.end(),  by_path);
		std::sort(files.begin(), files.end(), by_path);

		m_entries.insert(m_entries.end(), dirs.begin(),  dirs.end());
		m_entries.insert(m_entries.end(), files.begin(), files.end());
	}

	// ─── left panel: directory tree ───────────────────────────────────────────

	void asset_browser_panel::draw_dir_tree(const std::filesystem::path& dir, int depth)
	{
		if (depth > 6)
			return;

		std::error_code ec;
		std::vector<std::filesystem::path> subdirs;
		for (auto it = std::filesystem::directory_iterator(dir, ec);
		     !ec && it != std::filesystem::directory_iterator{};
		     it.increment(ec))
		{
			std::error_code ec2;
			if (it->is_directory(ec2) && !ec2)
				subdirs.push_back(it->path());
		}
		std::sort(subdirs.begin(), subdirs.end());

		for (const auto& sub : subdirs) {
			const std::string name = sub.filename().string();
			const bool selected  = (sub == m_current_dir);
			// Auto-expand if m_current_dir is inside this subtree
			const bool is_ancestor = [&] {
				auto p = m_current_dir;
				while (p != p.parent_path()) {
					if (p == sub) return true;
					p = p.parent_path();
				}
				return false;
			}();

			ImGuiTreeNodeFlags flags =
				ImGuiTreeNodeFlags_OpenOnArrow |
				ImGuiTreeNodeFlags_SpanFullWidth;
			if (selected)   flags |= ImGuiTreeNodeFlags_Selected;
			if (is_ancestor) ImGui::SetNextItemOpen(true, ImGuiCond_Once);

			const bool opened = ImGui::TreeNodeEx(name.c_str(), flags);

			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				navigate_to(sub);

			if (opened) {
				draw_dir_tree(sub, depth + 1);
				ImGui::TreePop();
			}
		}
	}

	// ─── render ───────────────────────────────────────────────────────────────

	void asset_browser_panel::on_render()
	{
		if (m_base_path.empty()) {
			ImGui::TextDisabled("(base path not set)");
			return;
		}

		// ── breadcrumb ───────────────────────────────────────────────────────
		{
			// Root button
			if (ImGui::SmallButton("res://"))
				navigate_to(m_base_path);

			// One button per path segment between base and current
			auto rel = m_current_dir.lexically_relative(m_base_path);
			std::filesystem::path cursor = m_base_path;
			for (const auto& part : rel) {
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
			draw_dir_tree(m_base_path);
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();

		ImGui::NextColumn();

		// Right: current-directory contents
		if (m_needs_refresh)
			refresh_entries();

		// ".." back button (disabled at root)
		{
			bool at_root = (m_current_dir == m_base_path);
			if (at_root) ImGui::BeginDisabled();
			if (ImGui::Selectable("[/] .."))
				navigate_to(m_current_dir.parent_path());
			if (at_root) ImGui::EndDisabled();
		}

		for (const auto& e : m_entries) {
			ImGui::Selectable(e.label.c_str());

			if (e.is_dir) {
				// Navigate into folder on click
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					navigate_to(e.path);
			} else {
				// Double-click on file
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					if (m_on_file_double_clicked)
						m_on_file_double_clicked(e.path);
				}

				// Drag source: full absolute path
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
					const std::string full = e.path.string();
					ImGui::SetDragDropPayload("ASSET_PATH", full.c_str(), full.size() + 1);
					ImGui::TextUnformatted(e.label.c_str());
					ImGui::EndDragDropSource();
				}

				// Context menu
				if (ImGui::BeginPopupContextItem()) {
					if (ImGui::MenuItem("Copy path"))
						ImGui::SetClipboardText(e.path.string().c_str());
					if (ImGui::MenuItem("Copy relative path")) {
						auto rel = e.path.lexically_relative(m_base_path);
						ImGui::SetClipboardText(rel.string().c_str());
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
