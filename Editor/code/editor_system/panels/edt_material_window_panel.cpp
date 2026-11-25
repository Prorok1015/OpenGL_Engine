#include "edt_material_window_panel.h"
#include "edt_shader_problems.h"
#include "edt_ui_metrics.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/gui/gui_backend_interface.h>
#include <snk/gui/gui_system.h>
#include <snk/gui/gui_theme.h>
#include <snk/platform/plt_shell_open.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_system.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <imgui.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace
{
	constexpr std::array<edt::preview_shape, 3> k_shapes{ edt::preview_shape::sphere, edt::preview_shape::cube, edt::preview_shape::plane };
}

edt::material_window_panel::material_window_panel(data_document& document, editor_services& services, document_id id, bool quiet,
	preview_view view)
	: data_document_panel(document, services, id, quiet)
	, m_render(view.render)
	, m_gui(view.gui)
	, m_editor_services(services)
	, m_model(document, services)
	, m_slots(document)
	, m_content(document, m_model, services)
	, m_stage(services, m_content, view)
{
}

void edt::material_window_panel::on_hidden()
{
	m_stage.park();
	data_document_panel::on_hidden();
}

void edt::material_window_panel::draw_before_fields()
{
	m_stage.update();
	if (m_content.sky_material()) {
		ImGui::TextDisabled("Sky material: no preview.");
		ImGui::Separator();
		return;
	}
	ImGui::SetNextItemWidth(em(110.f));
	if (ImGui::BeginCombo("Shape", std::string{ preview_shape_label(m_content.shape()) }.c_str())) {
		for (const preview_shape shape : k_shapes) {
			const bool selected = shape == m_content.shape();
			if (ImGui::Selectable(std::string{ preview_shape_label(shape) }.c_str(), selected) && !selected) {
				m_content.set_shape(shape);
				m_stage.sync();
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	bool sky = m_stage.sky();
	if (ImGui::Checkbox("Sky", &sky)) {
		m_stage.set_sky(sky);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The sky behind the shape, or the camera's plain clear.");
	}
	// Resizable from its bottom edge; the fields scroll under it.
	if (ImGui::BeginChild("##preview", ImVec2(0.f, em(220.f)), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		m_stage.draw_image();
	} else {
		m_stage.park();
	}
	ImGui::EndChild();
}

void edt::material_window_panel::draw_after_fields()
{
	draw_textures();
	draw_defines();
	draw_compile_errors();
}

void edt::material_window_panel::draw_textures()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Textures");
	const picker::catalog* catalog = asset_catalog();
	for (const texture_slot& slot : material_texture_slots()) {
		ImGui::PushID(static_cast<int>(slot.index));
		draw_texture_slot(slot, catalog);
		ImGui::PopID();
	}
}

void edt::material_window_panel::draw_texture_slot(const texture_slot& slot, const picker::catalog* catalog)
{
	schema_ui::map_edit_state& state = map_state();
	const ImGuiID slot_id = ImGui::GetID("##slot");
	const texture_slot_state current = m_slots.state(slot);

	draw_slot_thumbnail(current.text);
	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::TextUnformatted(std::string(slot.name).c_str());
	if (current.inherited) {
		ImGui::SameLine();
		ImGui::TextDisabled("(inherited)");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Set by a parent material; a change here writes this material's own list.");
		}
	}

	const bool can_pick = catalog && catalog->ready();
	ImGui::BeginDisabled(!can_pick);
	if (ImGui::SmallButton("...")) {
		ImGui::OpenPopup(schema_ui::k_picker_popup);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", can_pick ? "Pick a texture (a .txm.desc)." : "The asset list is not ready.");
	}
	ImGui::SameLine();
	// The reference is the drop target; a click opens the picker as "..." does.
	const std::string shown = current.text.empty() ? std::string{ "(none)" } : current.text;
	if (ImGui::Button((shown + "###reference").c_str()) && can_pick) {
		ImGui::OpenPopup(schema_ui::k_picker_popup);
	}
	if (const json::object* reference = current.value.if_object(); reference && ImGui::IsItemHovered()) {
		if (const json::value* guid = reference->if_contains("guid"); guid && guid->is_string()) {
			ImGui::SetTooltip("ID %s", std::string(guid->as_string()).c_str());
		}
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
			// The browser sends the path below res:// with its terminating zero.
			if (payload->DataSize > 0) {
				const std::string path(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize) - 1);
				const picker::texture_choice choice = picker::choose_texture(path, catalog, m_picture_texture);
				state.drop_notes.erase(slot_id);
				if (choice.answer.outcome == picker::verdict::result::rejected) {
					state.drop_refusals[slot_id] = std::format("'{}': {}", path, choice.answer.reason);
				} else {
					state.drop_refusals.erase(slot_id);
					if (choice.answer.outcome == picker::verdict::result::unknown) {
						state.drop_notes[slot_id] = std::format("'{}' {}", path, choice.answer.reason);
					}
					m_slots.set(slot, choice.value);
				}
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(current.value.is_null());
	if (ImGui::SmallButton("Clear")) {
		state.drop_refusals.erase(slot_id);
		state.drop_notes.erase(slot_id);
		m_slots.clear(slot);
	}
	ImGui::EndDisabled();

	boost::json::value picked;
	if (catalog && schema_ui::draw_picker_popup(picker::texture_filter(), *catalog, state, slot_id, picked)) {
		state.drop_refusals.erase(slot_id);
		state.drop_notes.erase(slot_id);
		m_slots.set(slot, std::move(picked));
	}
	if (!ImGui::IsPopupOpen(schema_ui::k_picker_popup)) {
		state.picker_pages.erase(slot_id);
		state.picker_queries.erase(slot_id);
	}
	if (const auto refused = state.drop_refusals.find(slot_id); refused != state.drop_refusals.end()) {
		ImGui::TextColored(colors().error, "%s", refused->second.c_str());
	}
	if (const auto noted = state.drop_notes.find(slot_id); noted != state.drop_notes.end()) {
		ImGui::TextDisabled("%s", noted->second.c_str());
	}
	if (catalog && !current.text.empty()) {
		const picker::reference_state verdict = picker::judge_text(picker::texture_filter(), current.text, *catalog);
		if (verdict.severity == picker::reference_state::level::error) {
			ImGui::TextColored(colors().error, "%s", verdict.text.c_str());
		} else if (verdict.severity == picker::reference_state::level::note) {
			ImGui::TextDisabled("%s", verdict.text.c_str());
		}
	}
	ImGui::EndGroup();
}

void edt::material_window_panel::draw_slot_thumbnail(const std::string& texture)
{
	const ImVec2 size(em(48.f), em(48.f));
	gui::imgui_backend_interface* backend = m_gui ? m_gui->get_backend_interface() : nullptr;
	if (m_render && backend && !texture.empty()) {
		rnd::texture_manager& textures = m_render->get_texture_manager();
		const rnd::texture_id id = textures.require(res::tag{ texture });
		if (textures.is_ready(id)) {
			if (const auto handle = textures.binding(id).handle) {
				// Loaded pictures are bottom-up, as the GL textures are.
				ImGui::Image(backend->to_imgui(handle), size, ImVec2(0, 1), ImVec2(1, 0));
				return;
			}
		}
	}
	ImGui::Dummy(size);
}

void edt::material_window_panel::draw_defines()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Defines");
	ImGui::BeginDisabled(!m_model.can_reset_to_inherited());
	if (ImGui::SmallButton("Reset to inherited")) {
		m_model.reset_to_inherited();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Drop this material's own defines: the parent's list applies again.");
	}
	const material_shader& shader = m_model.shader();
	if (shader.shader.is_valid()) {
		ImGui::TextDisabled("shader: %s", std::string{ shader.shader.view() }.c_str());
	} else {
		ImGui::TextDisabled("The material has no shader: only its own defines are listed.");
	}
	if (!shader.error.empty()) {
		ImGui::TextColored(colors().warning, "%s", shader.error.c_str());
	}

	const std::vector<define_row> rows = m_model.rows();
	std::optional<std::string> toggled;
	if (rows.empty()) {
		ImGui::TextDisabled("No defines: the shader tests none and the material sets none.");
	} else if (ImGui::BeginTable("##defines", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Define", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, em(110.f));
		ImGui::TableHeadersRow();
		for (const define_row& row : rows) {
			ImGui::TableNextRow();
			ImGui::PushID(row.name.c_str());
			ImGui::TableSetColumnIndex(0);
			bool on = row.state != define_state::off;
			if (ImGui::Checkbox(row.name.c_str(), &on)) {
				toggled = row.name;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", row.state == define_state::own_on ? "Set in this material's own defines."
					: row.state == define_state::inherited_on ? "Set by a parent material; turning it off writes this material's own list."
					: "Off.");
			}
			if (row.state == define_state::inherited_on) {
				ImGui::SameLine();
				ImGui::TextDisabled("(inherited)");
			} else if (row.inherited && row.state == define_state::off) {
				ImGui::SameLine();
				ImGui::TextDisabled("(inherited, off here)");
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%s", row.from_shader ? "shader" : "material");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	// After the table: the toggle changes the rows the loop drew.
	if (toggled) {
		m_model.toggle(*toggled);
	}
	draw_add_define();
}

void edt::material_window_panel::draw_compile_errors()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Compile errors");
	m_model.scan();
	rnd::render_system* render = m_render;
	if (render && render->get_driver()) {
		m_model.preprocess(std::chrono::steady_clock::now(), render->get_driver()->bindings());
	}
	const compile_errors& errors = m_model.errors(render ? render->shader_problems_generation() : 0,
		[render] { return render ? render->shader_problems() : std::vector<rnd::shader_problem>{}; }, m_content.material_tag());

	for (const std::string& warning : m_model.pre_check()) {
		ImGui::TextColored(colors().warning, "%s", warning.c_str());
	}
	if (errors.sky) {
		ImGui::TextDisabled("From the last frame the level drew; no preview for sky materials.");
	}
	if (errors.material.empty()) {
		ImGui::TextDisabled("This material's variants: no errors.");
	} else {
		ImGui::TextUnformatted("This material");
		for (std::size_t i = 0; i < errors.material.size(); ++i) {
			draw_compile_row(errors.material[i], static_cast<int>(i));
		}
	}
	if (!errors.others.empty()) {
		const std::string header = std::format("Other variants of this shader ({})###other_variants", errors.others.size());
		if (ImGui::CollapsingHeader(header.c_str())) {
			for (std::size_t i = 0; i < errors.others.size(); ++i) {
				draw_compile_row(errors.others[i], static_cast<int>(errors.material.size() + i));
			}
		}
	}
	if (!m_go_to_error.empty()) {
		ImGui::TextColored(colors().error, "%s", m_go_to_error.c_str());
	}
}

void edt::material_window_panel::draw_compile_row(const compile_row& row, int index)
{
	ImGui::PushID(index);
	const rnd::shader_problem& item = row.problem;
	if (ImGui::SmallButton("Go to")) {
		const std::vector<problem> rows = shader_problem_publisher::to_problems({ item });
		res::resource_system* resources = m_editor_services.resources();
		const auto disk_path = [resources](const res::tag& file) -> std::optional<std::filesystem::path> {
			if (!resources) return std::nullopt;
			const std::optional<res::disk_file> on_disk = resources->disk_file_of(file);
			return on_disk ? std::optional<std::filesystem::path>{ on_disk->file } : std::nullopt;
		};
		std::string open_error;
		const shader_file_open opened = rows.empty() ? shader_file_open::not_a_shader_source
			: open_shader_problem_file(rows.front(), disk_path, [&open_error](const std::filesystem::path& file) {
				open_error = platform::open_in_shell(file);
				return open_error;
			});
		const std::string file = rows.empty() ? std::string{} : rows.front().file;
		switch (opened) {
		case shader_file_open::opened: m_go_to_error.clear(); break;
		case shader_file_open::not_a_shader_source: m_go_to_error = std::format("Not opened: '{}' is not a shader source.", file); break;
		case shader_file_open::not_on_disk: m_go_to_error = std::format("Not opened: '{}' is not a file on the disk.", file); break;
		case shader_file_open::failed: m_go_to_error = std::format("Cannot open '{}': {}", file, open_error); break;
		}
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Open the file in the system's app for it.");
	}
	ImGui::SameLine();
	std::string place = item.file.is_valid() ? std::string{ item.file.view() } : std::string{ "(no file)" };
	if (item.line > 0) place += std::format(":{}", item.line);
	ImGui::TextColored(colors().error, "%s", place.c_str());
	std::string labels;
	if (row.skinned) labels += " [skinned]";
	if (row.preprocess) labels += " [preprocess]";
	if (!labels.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", labels.c_str());
	}
	ImGui::Indent();
	ImGui::TextWrapped("%s", item.message.c_str());
	if (!row.define_set.empty()) {
		ImGui::TextDisabled("defines: %s", row.define_set.c_str());
	}
	ImGui::Unindent();
	ImGui::PopID();
}

void edt::material_window_panel::draw_add_define()
{
	ImGui::SetNextItemWidth(em(180.f));
	const bool entered = ImGui::InputTextWithHint("##new_define", "NEW_DEFINE", m_new_define, sizeof(m_new_define),
		ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if (ImGui::Button("Add") || entered) {
		m_add_error = m_model.add(m_new_define);
		if (m_add_error.empty()) {
			m_new_define[0] = '\0';
		}
	}
	if (!m_add_error.empty()) {
		ImGui::TextColored(colors().error, "%s", m_add_error.c_str());
	}
}
