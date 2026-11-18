#include <snk/common/fs_paths.h>
#include "edt_asset_browser_panel.h"
#include "edt_prefab_variant_creator.h"
#include "edt_code_section.h"
#include "edt_document_creator.h"
#include "edt_ui_probe_imgui.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_ui_font.h>
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

namespace edt
{
	// ─── helpers ──────────────────────────────────────────────────────────────

	static std::string to_lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	struct file_icon {
		const char* glyph;
		const char* text;
	};

	static file_icon icon_of_extension(const std::string& ext)
	{
		if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx") return {gui::icon::model, "[3D] "};
		if (ext == ".desc")                                                      return {gui::icon::desc, "[D]  "};
		if (ext == ".png"  || ext == ".jpg"  || ext == ".jpeg" ||
		    ext == ".tga"  || ext == ".hdr"  || ext == ".bmp")                  return {gui::icon::image, "[IMG]"};
		if (ext == ".vert" || ext == ".frag" || ext == ".geom" || ext == ".glsl") return {gui::icon::file_code, "[SHD]"};
		if (ext == ".wav"  || ext == ".ogg"  || ext == ".mp3")                  return {gui::icon::sound, "[SND]"};
		return {gui::icon::file, "[F]  "};
	}

	static const char* folder_icon()
	{
		return gui::ui_font_has_icons() ? gui::icon::folder : "[/]";
	}

	const char* asset_browser_panel::get_file_icon(const std::filesystem::path& p) const
	{
		const file_icon icon = icon_of_extension(to_lower(fs_paths::utf8_of(p.extension())));
		return gui::ui_font_has_icons() ? icon.glyph : icon.text;
	}

	// ─── lifecycle ────────────────────────────────────────────────────────────

	asset_browser_panel::asset_browser_panel()
		: panel_base("asset_browser", "Asset Browser")
	{
	}

	void asset_browser_panel::on_hidden()
	{
		const ui_probe::scoped_pass probe_pass(m_ui_probe.get(), {"asset_browser", "asset_browser_search"});
	}

	void asset_browser_panel::set_layers(std::vector<std::filesystem::path> layers)
	{
		m_layers = std::move(layers);
		m_current_rel.clear();
		m_needs_refresh = true;
	}

	asset_browser_panel::~asset_browser_panel() = default;

	void asset_browser_panel::set_document_creator(std::shared_ptr<document_creator> creator, std::function<void(const res::tag&)> on_created)
	{
		m_creator = std::move(creator);
		m_create_form = m_creator ? std::make_unique<document_create_form>(*m_creator) : nullptr;
		m_on_document_created = std::move(on_created);
	}

	void asset_browser_panel::draw_create_menu_item(const std::filesystem::path& folder)
	{
		if (!m_create_form) {
			return;
		}
		const bool no_types = m_creator->types().empty();
		ImGui::BeginDisabled(no_types);
		if (ImGui::MenuItem("Create Document...")) {
			m_create_form->open(fs_paths::generic_utf8_of(folder));
			m_create_requested = true;
		}
		ImGui::EndDisabled();
		if (no_types && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("no desc type is registered as a document");
		}
	}

	void asset_browser_panel::draw_create_window()
	{
		if (!m_create_form) {
			return;
		}
		if (m_create_requested) {
			ImGui::OpenPopup("Create Document");
			m_create_requested = false;
		}
		if (!ImGui::BeginPopupModal("Create Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}
		document_create_form& form = *m_create_form;
		ImGui::TextDisabled("Folder: res://%s", form.folder().c_str());
		const auto& types = form.types();
		const char* preview = types.empty() ? "(no document types)" : types[form.type_index()].c_str();
		if (ImGui::BeginCombo("Type", preview)) {
			for (std::size_t i = 0; i < types.size(); ++i) {
				if (ImGui::Selectable(types[i].c_str(), i == form.type_index())) {
					form.set_type_index(i);
				}
			}
			ImGui::EndCombo();
		}
		std::vector<char> buffer(std::max<std::size_t>(form.name().size() + 64, 256), '\0');
		std::copy(form.name().begin(), form.name().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool entered = ImGui::InputText("Name", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.name()) {
			form.set_name(buffer.data());
		}
		ImGui::TextDisabled(".desc is added when the name has no extension");
		if (const std::string problem = form.problem(); !problem.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", problem.c_str());
			ImGui::PopStyleColor();
		}
		if (!form.error().empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", form.error().c_str());
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(!form.can_submit());
		const bool clicked = ImGui::Button("Create");
		ImGui::EndDisabled();
		if ((clicked || (entered && form.can_submit())) && form.can_submit()) {
			if (const std::optional<res::tag> made = form.submit()) {
				ImGui::CloseCurrentPopup();
				if (m_on_document_created) {
					m_on_document_created(*made);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void asset_browser_panel::set_variant_creator(std::shared_ptr<prefab_variant_creator> creator, std::function<void(const res::tag&)> on_created)
	{
		m_variant_creator = std::move(creator);
		m_variant_form = m_variant_creator ? std::make_unique<prefab_variant_form>(*m_variant_creator) : nullptr;
		m_on_variant_created = std::move(on_created);
	}

	void asset_browser_panel::draw_variant_window()
	{
		if (!m_variant_form) {
			return;
		}
		constexpr const char* title = "Create Prefab Variant";
		if (m_variant_requested) {
			ImGui::OpenPopup(title);
			m_variant_requested = false;
		}
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}
		prefab_variant_form& form = *m_variant_form;
		ImGui::TextDisabled("Base: %s", std::string(form.base().view()).c_str());
		std::vector<char> buffer(std::max<std::size_t>(form.name().size() + 64, 256), '\0');
		std::copy(form.name().begin(), form.name().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool entered = ImGui::InputText("Name", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.name()) {
			form.set_name(buffer.data());
		}
		ImGui::TextDisabled(".prefab.desc is added; the file lands next to the base");
		for (const std::string* text : { &form.problem(), &form.error() }) {
			if (text->empty()) continue;
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", text->c_str());
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(!form.can_submit());
		const bool clicked = ImGui::Button("Create");
		ImGui::EndDisabled();
		if ((clicked || entered) && form.can_submit()) {
			if (const std::optional<res::tag> made = form.submit()) {
				ImGui::CloseCurrentPopup();
				if (m_on_variant_created) {
					m_on_variant_created(*made);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
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
		m_labels_have_icons = gui::ui_font_has_icons();

		const std::string search_lower = to_lower(m_search);

		for (auto& src : list_merged_directory(m_layers, m_current_rel)) {
			// Search filter (dirs always pass)
			if (!search_lower.empty() && !src.is_dir) {
				if (to_lower(src.name).find(search_lower) == std::string::npos)
					continue;
			}
			std::string label = src.is_dir
				? std::string(folder_icon()) + " " + src.name
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
			if (m_create_form && ImGui::BeginPopupContextItem()) {
				draw_create_menu_item(sub.relative);
				ImGui::EndPopup();
			}

			if (opened) {
				draw_dir_tree(sub.relative, depth + 1);
				ImGui::TreePop();
			}
		}
	}

	// ─── render ───────────────────────────────────────────────────────────────

	void asset_browser_panel::on_render()
	{
		const ui_probe::scoped_pass search_pass(m_ui_probe.get(), {"asset_browser_search"});
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
				ImGui::SameLine(0.f, dpi_px(2.f));
				ImGui::TextDisabled(">");
				ImGui::SameLine(0.f, dpi_px(2.f));
				const std::string seg = fs_paths::utf8_of(part);
				if (ImGui::SmallButton(seg.c_str()))
					navigate_to(cursor);
			}
		}

		// ── search ───────────────────────────────────────────────────────────
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::InputTextWithHint("##search", "Search files...", m_search, sizeof(m_search)))
			m_needs_refresh = true;
		edt::probe_last_item(m_ui_probe.get(), "asset_browser_search", "search");

		ImGui::Separator();

		// ── two columns: tree | files ─────────────────────────────────────────
		ImGui::Columns(2, "ab_cols", true);
		ImGui::SetColumnWidth(0, dpi_px(160.f));

		// Left: directory tree
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, dpi_px(12.f));
		if (ImGui::TreeNodeEx("res://##directorytreeroot", ImGuiTreeNodeFlags_DefaultOpen |
		                                ImGuiTreeNodeFlags_SpanFullWidth))
		{
			draw_dir_tree({});
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();

		if (m_code_section)
			m_code_section->render(colors());

		ImGui::NextColumn();

		// Right: current-directory contents
		if (m_needs_refresh || m_labels_have_icons != gui::ui_font_has_icons())
			refresh_entries();

		// Room under the list for "Used by" of the picked file.
		const bool shows_usages = !m_selected_rel.empty() && m_usages.has_source();
		const float usages_height = shows_usages ? ImGui::GetTextLineHeightWithSpacing() * 10.f : 0.f;
		if (ImGui::BeginChild("##asset_files", ImVec2(0.f, -usages_height), ImGuiChildFlags_None)) {
			// ".." back button (disabled at root)
			{
				bool at_root = m_current_rel.empty();
				if (at_root) ImGui::BeginDisabled();
				if (ImGui::Selectable((std::string(folder_icon()) + " ..").c_str()))
					navigate_to(m_current_rel.parent_path());
				if (at_root) ImGui::EndDisabled();
			}

			if (m_ui_probe)
				m_ui_probe->begin_pass("asset_browser");

			const size_t top_layer = m_layers.size() - 1;
			for (const auto& e : m_entries) {
				// Entries that come from a lower layer are dimmed; the tooltip names the layer root.
				const bool from_lower_layer = e.src.layer != top_layer;
				if (from_lower_layer)
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				const bool picked = ImGui::Selectable(e.label.c_str(), !e.src.is_dir && e.src.relative == m_selected_rel);
				if (m_ui_probe) {
					// A row the list cuts off is "visible" to ImGui; only its part inside the list counts, so a
					// driver that aims at the centre does not land below the list.
					const ImDrawList* list = ImGui::GetWindowDrawList();
					const ImVec2 item_min{ std::max(ImGui::GetItemRectMin().x, list->GetClipRectMin().x),
						std::max(ImGui::GetItemRectMin().y, list->GetClipRectMin().y) };
					const ImVec2 item_max{ std::min(ImGui::GetItemRectMax().x, list->GetClipRectMax().x),
						std::min(ImGui::GetItemRectMax().y, list->GetClipRectMax().y) };
					if (ImGui::IsItemVisible() && item_max.x - item_min.x >= 2.f && item_max.y - item_min.y >= 2.f) {
						m_ui_probe->report("asset_browser", e.src.name, item_min.x, item_min.y, item_max.x, item_max.y);
					} else {
						m_ui_probe->report_hidden("asset_browser", e.src.name);
					}
				}
				if (from_lower_layer)
					ImGui::PopStyleColor();
				if (ImGui::IsItemHovered() && m_layers.size() > 1)
					ImGui::SetTooltip("layer %zu: %s", e.src.layer, fs_paths::utf8_of(m_layers[e.src.layer]).c_str());

				if (e.src.is_dir) {
					// Navigate into folder on click
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						navigate_to(e.src.relative);
					if (m_create_form && ImGui::BeginPopupContextItem()) {
						draw_create_menu_item(e.src.relative);
						ImGui::EndPopup();
					}
				} else {
					if (picked)
						m_selected_rel = e.src.relative;
					// Double-click on file
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						if (m_on_file_double_clicked)
							m_on_file_double_clicked(e.src.relative);
					}

					// Drag source: path relative to res:// (the physical path would be wrong for lower layers)
					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
						const std::string full = fs_paths::generic_utf8_of(e.src.relative);
						ImGui::SetDragDropPayload("ASSET_PATH", full.c_str(), full.size() + 1);
						ImGui::TextUnformatted(e.label.c_str());
						ImGui::EndDragDropSource();
					}

					// Context menu
					if (ImGui::BeginPopupContextItem()) {
						if (m_operation_form) {
							if (ImGui::MenuItem("Rename...")) ask_operation(asset_operation::rename, e.src.relative);
							if (ImGui::MenuItem("Move...")) ask_operation(asset_operation::move, e.src.relative);
							if (ImGui::MenuItem("Delete...")) ask_operation(asset_operation::remove, e.src.relative);
							ImGui::Separator();
						}
						if (ImGui::MenuItem("Copy path"))
							ImGui::SetClipboardText(fs_paths::utf8_of(e.src.path).c_str());
						if (ImGui::MenuItem("Copy relative path")) {
							ImGui::SetClipboardText(fs_paths::generic_utf8_of(e.src.relative).c_str());
						}
						if (m_variant_form && e.src.relative.generic_string().ends_with(".prefab.desc") && ImGui::MenuItem("Create Variant")) {
							m_variant_form->open(res::tag::make(e.src.relative.generic_string()));
							m_variant_requested = true;
						}
						ImGui::EndPopup();
					}
				}
			}

			if (m_ui_probe)
				m_ui_probe->end_pass("asset_browser");

			if (m_entries.empty())
				ImGui::TextDisabled("(empty)");

			// The blank of the open folder.
			if (m_create_form && ImGui::BeginPopupContextWindow("##asset_browser_blank", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
				draw_create_menu_item(m_current_rel);
				ImGui::EndPopup();
			}
		}
		ImGui::EndChild();

		draw_usages();

		ImGui::Columns(1);

		draw_create_window();
		draw_variant_window();
		draw_operation_window();
	}
}

void edt::asset_browser_panel::draw_usages()
{
	if (m_selected_rel.empty() || !m_usages.has_source())
		return;
	ImGui::Separator();
	if (ImGui::BeginChild("##asset_usages", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
		draw_usage_rows();
	ImGui::EndChild();
}

void edt::asset_browser_panel::draw_usage_rows()
{
	const std::string selected = fs_paths::generic_utf8_of(m_selected_rel);
	ImGui::Text("Used by: %s", selected.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("(saved files)");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Found in the files as they are on the disk: an unsaved change is not counted.");

	const usage_report* report = m_usages.report_for(res::tag::make(selected));
	if (!report) {
		ImGui::TextDisabled("No asset database: no project is open.");
		return;
	}
	if (report->complete && !report->found) {
		ImGui::SameLine();
		ImGui::TextDisabled("(missing)");
	}
	if (!report->complete)
		ImGui::TextDisabled("Still reading files: the list may grow.");
	if (report->items.empty())
		ImGui::TextDisabled("Nothing refers to it.");
	const std::vector<std::string>& rows = m_usages.rows();
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows.size()));
	while (clipper.Step()) {
		for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
			const asset_usage& usage = report->items[static_cast<std::size_t>(index)];
			// The row is drawn as plain text over an unnamed selectable: a "##" in a file name or a JSON key stays text.
			const ImVec2 row_start = ImGui::GetCursorPos();
			ImGui::PushID(index);
			ImGui::Selectable("##usage");
			ImGui::PopID();
			const bool hovered = ImGui::IsItemHovered();
			const bool opened = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			ImGui::SetCursorPos(row_start);
			// Found by its text, not through a field of a schema: it may be no reference at all.
			const bool loose = usage.kind == usage_kind::loose;
			if (loose)
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(rows[static_cast<std::size_t>(index)].c_str());
			if (loose)
				ImGui::PopStyleColor();
			if (hovered)
				ImGui::SetTooltip("%s", loose ? "Not a field of a schema (a custom field, the record of a prefab instance): found by its text."
					: usage.by_guid ? "Refers to it by its ID." : "Refers to it by its path.");
			constexpr std::string_view protocol = "res://";
			if (opened && m_on_file_double_clicked && usage.file.starts_with(protocol))
				m_on_file_double_clicked(fs_paths::path_from_utf8(std::string_view(usage.file).substr(protocol.size())));
		}
	}
	if (!report->skipped.empty()) {
		ImGui::TextDisabled("%zu file(s) too large to read for references", report->skipped.size());
		if (ImGui::IsItemHovered()) {
			std::string names;
			for (const std::string& file : report->skipped)
				names += file + '\n';
			ImGui::SetTooltip("%s", names.c_str());
		}
	}
}

void edt::asset_browser_panel::set_asset_operations(asset_operation_ports ports)
{
	m_operation_form = std::make_unique<asset_operation_form>(std::move(ports));
}

void edt::asset_browser_panel::ask_operation(asset_operation what, const std::filesystem::path& relative)
{
	m_operation_form->open(what, res::tag::make(fs_paths::generic_utf8_of(relative)));
	m_operation_requested = true;
}

namespace
{
	std::vector<std::string> lines_of(const std::string& text)
	{
		std::vector<std::string> out;
		std::size_t start = 0;
		while (start <= text.size()) {
			const std::size_t end = text.find('\n', start);
			out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return out;
	}

	const char* verb_of(edt::asset_operation what)
	{
		switch (what) {
		case edt::asset_operation::rename: return "Rename";
		case edt::asset_operation::move: return "Move";
		case edt::asset_operation::remove: return "Delete";
		}
		return "Change";
	}
}

void edt::asset_browser_panel::draw_operation_lines(const char* id, const std::vector<std::string>& lines, bool error)
{
	if (ImGui::BeginChild(id, ImVec2(em(480.f), em(lines.size() > 8 ? 220.f : 120.f)), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
		if (error) ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
		for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
		if (error) ImGui::PopStyleColor();
	}
	ImGui::EndChild();
}

void edt::asset_browser_panel::draw_operation_window()
{
	if (!m_operation_form) return;
	asset_operation_form& form = *m_operation_form;
	constexpr const char* title = "Asset Operation";
	if (m_operation_requested) {
		ImGui::OpenPopup(title);
		m_operation_requested = false;
	}
	if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	switch (form.current()) {
	case asset_operation_form::stage::editing:
		draw_operation_editing(form);
		break;
	case asset_operation_form::stage::confirming_force:
		draw_operation_force(form);
		break;
	case asset_operation_form::stage::reported:
		draw_operation_report(form);
		break;
	case asset_operation_form::stage::closed:
		ImGui::CloseCurrentPopup();
		break;
	}
	ImGui::EndPopup();
}

void edt::asset_browser_panel::draw_operation_editing(asset_operation_form& form)
{
	const char* verb = verb_of(form.operation());
	ImGui::Text("%s %s", verb, std::string(form.asset().view()).c_str());
	bool entered = false;
	if (form.operation() != asset_operation::remove) {
		std::vector<char> buffer(std::max<std::size_t>(form.input().size() + 64, 256), '\0');
		std::copy(form.input().begin(), form.input().end(), buffer.begin());
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(em(480.f));
		const char* label = form.operation() == asset_operation::rename ? "New name" : "Folder below res://";
		entered = ImGui::InputText(label, buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (buffer.data() != form.input()) form.set_input(buffer.data());
	}
	if (form.refused()) {
		draw_operation_lines("##operation_refusal", lines_of(form.refusal_text()), true);
	} else {
		draw_operation_lines("##operation_preview", form.preview_lines(), false);
	}
	if (form.needs_force() && !form.refused()) {
		ImGui::TextUnformatted("Other files refer to it, so Delete stays off.");
	}
	if (!form.error().empty()) draw_operation_lines("##operation_error", lines_of(form.error()), true);

	ImGui::BeginDisabled(!form.can_submit());
	const bool clicked = ImGui::Button(verb);
	ImGui::EndDisabled();
	if ((clicked || (entered && form.can_submit())) && form.submit()) {
		m_needs_refresh = true;
		m_selected_rel.clear();
	}
	if (form.can_ask_force()) {
		ImGui::SameLine();
		if (ImGui::Button("Delete anyway (force)...")) form.ask_force();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		form.close();
		ImGui::CloseCurrentPopup();
	}
}

void edt::asset_browser_panel::draw_operation_force(asset_operation_form& form)
{
	ImGui::Text("Delete %s although other files refer to it?", std::string(form.asset().view()).c_str());
	draw_operation_lines("##operation_force", lines_of(form.force_text()), false);
	if (!form.error().empty()) draw_operation_lines("##operation_error", lines_of(form.error()), true);
	ImGui::PushStyleColor(ImGuiCol_Text, colors().warning);
	const bool clicked = ImGui::Button("Delete and break the references");
	ImGui::PopStyleColor();
	if (clicked && form.submit_force()) {
		m_needs_refresh = true;
		m_selected_rel.clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape)) form.back();
}

void edt::asset_browser_panel::draw_operation_report(asset_operation_form& form)
{
	ImGui::TextUnformatted(form.report_title().c_str());
	draw_operation_lines("##operation_report", form.report_lines(), false);
	if (form.undone()) ImGui::TextDisabled("Undone: the files are back as they were.");
	if (!form.undo_error().empty()) draw_operation_lines("##operation_undo_error", lines_of(form.undo_error()), true);
	ImGui::BeginDisabled(!form.can_undo());
	const bool undo = ImGui::Button("Undo");
	ImGui::EndDisabled();
	if (undo && form.undo()) m_needs_refresh = true;
	ImGui::SameLine();
	if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		form.close();
		ImGui::CloseCurrentPopup();
	}
}
