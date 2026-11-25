#include "edt_texture_window_panel.h"
#include "edt_data_document.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_theme.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <imgui.h>
#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace
{
	constexpr std::size_t k_picker_rows = 200;

	std::string_view format_name(rnd::driver::texture_header::TYPE format)
	{
		using type = rnd::driver::texture_header::TYPE;
		switch (format) {
		case type::R8: return "R8";
		case type::RGB8: return "RGB8";
		case type::RGBA8: return "RGBA8";
		case type::RGBA12: return "RGBA12";
		case type::RGBA16: return "RGBA16";
		case type::RGBA16F: return "RGBA16F";
		case type::RGBA32F: return "RGBA32F";
		case type::R32I: return "R32I";
		case type::R32F: return "R32F";
		case type::D32F_S8: return "D32F_S8";
		case type::D24_S8: return "D24_S8";
		case type::D32: return "D32";
		case type::D24: return "D24";
		case type::D16: return "D16";
		case type::D32F: return "D32F";
		case type::S1: return "S1";
		case type::S4: return "S4";
		case type::S8: return "S8";
		}
		return "unknown";
	}

	// A combo over `options`; true when another one was picked (`picked` holds it).
	bool option_combo(const char* label, const std::string& current, std::span<const std::string_view> options, std::string& picked)
	{
		bool changed = false;
		ImGui::SetNextItemWidth(edt::em(180.f));
		if (ImGui::BeginCombo(label, current.c_str())) {
			for (const std::string_view option : options) {
				const bool selected = option == current;
				if (ImGui::Selectable(std::string(option).c_str(), selected) && !selected) {
					picked = std::string(option);
					changed = true;
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		return changed;
	}
}

std::string edt::texture_info_text(rnd::render_system* render, const res::tag& tag)
{
	if (!render) return {};
	rnd::texture_manager& textures = render->get_texture_manager();
	const rnd::texture_id id = textures.require(tag);
	if (!textures.is_ready(id)) return "loading...";
	const rnd::driver::texture_header& info = textures.info(id);
	return std::format("{}x{} {}", info.data.extent.width, info.data.extent.height, format_name(info.data.format));
}

void edt::draw_texture_preview(preview_stage& stage, rnd::render_system* render, const res::tag& texture, const texture_source& source,
	int mip_levels)
{
	bool sky = stage.sky();
	if (ImGui::Checkbox("Sky", &sky)) {
		stage.set_sky(sky);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The sky behind the plane, or the camera's plain clear.");
	}
	// Resizable from its bottom edge; what follows scrolls under it.
	if (ImGui::BeginChild("##preview", ImVec2(0.f, em(220.f)), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		stage.draw_image();
	} else {
		stage.park();
	}
	ImGui::EndChild();
	const std::string made = texture_info_text(render, texture);
	ImGui::TextDisabled("%s%ssource: %s, mip levels: %d", made.c_str(), made.empty() ? "" : "; ", texture_source_text(source).c_str(),
		mip_levels);
}

edt::texture_window_panel::texture_window_panel(data_document& document, editor_services& services, document_id id, bool quiet,
	preview_view view)
	: data_document_panel(document, services, id, quiet)
	, m_model(document)
	, m_render(view.render)
	, m_content(texture_preview_source{
		.body = [&document] { return boost::json::value(document.own()); },
		.generation = [&document] { return document.generation(); } }, services)
	, m_stage(services, m_content, view)
{
}

edt::texture_window_panel::~texture_window_panel()
{
	// The world and the copies go first: nothing samples the texture or reloads it once it is removed.
	m_stage.close();
	if (m_render) m_render->get_texture_manager().remove(m_content.texture_tag());
}

void edt::texture_window_panel::on_hidden()
{
	m_stage.park();
	data_document_panel::on_hidden();
}

void edt::texture_window_panel::draw_before_fields()
{
	m_stage.update();
	draw_texture_preview(m_stage, m_render, m_content.texture_tag(), m_model.source(), m_model.mip_levels());
}

void edt::texture_window_panel::draw_after_fields()
{
	ImGui::Spacing();
	ImGui::SeparatorText("Sampling");
	std::string picked;
	if (option_combo("Wrap", m_model.wrap(), texture_wrap_options(), picked)) m_model.set_wrap(picked);
	if (option_combo("Min filter", m_model.min_filter(), texture_filter_options(), picked)) m_model.set_min(picked);
	if (option_combo("Mag filter", m_model.mag_filter(), texture_filter_options(), picked)) m_model.set_mag(picked);
	int levels = m_model.mip_levels();
	ImGui::SetNextItemWidth(em(180.f));
	if (ImGui::InputInt("Mip levels", &levels, 1, 1, ImGuiInputTextFlags_EnterReturnsTrue)) {
		m_model.set_mip_levels(std::max(levels, 1));
	}
	if (m_model.source().from == texture_source::kind::inline_pixels) {
		ImGui::Spacing();
		ImGui::TextDisabled("The pixels are held in the file.");
		ImGui::SameLine();
		draw_file_picker();
	}
}

void edt::texture_window_panel::draw_file_picker()
{
	const picker::catalog* catalog = asset_catalog();
	ImGui::BeginDisabled(catalog == nullptr);
	if (ImGui::SmallButton("Use file...")) {
		ImGui::OpenPopup("##texture_files");
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", catalog ? "Name a picture file instead: the inline pixels go."
			: "No project is open: there are no picture files to pick.");
	}
	if (!catalog || !ImGui::BeginPopup("##texture_files")) return;
	if (ImGui::IsWindowAppearing()) {
		m_query.clear();
		ImGui::SetKeyboardFocusHere();
	}
	char buffer[256] = {};
	m_query.copy(buffer, sizeof(buffer) - 1);
	ImGui::SetNextItemWidth(em(360.f));
	if (ImGui::InputTextWithHint("##query", "search", buffer, sizeof(buffer))) {
		m_query = buffer;
	}
	const picker::filter wanted{ .kind = desc::field_kind::res_tag, .extensions = std::string(k_picture_extensions) };
	const picker::candidates_page& page = picker::cached_candidates(m_page, *catalog, wanted, m_query, k_picker_rows);
	ImGui::TextDisabled("%zu files", page.total);
	if (ImGui::BeginChild("##list", ImVec2(em(420.f), em(260.f)))) {
		for (std::size_t i = 0; i < page.items.size(); ++i) {
			const asset_record& record = *page.items[i].record;
			const std::string row = std::format("{}   {}", record.path.relative(), page.items[i].layer_label);
			ImGui::PushID(static_cast<int>(i));
			if (ImGui::Selectable(row.c_str())) {
				m_model.use_file(record.path);
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
		}
		if (page.total > page.items.size()) {
			ImGui::TextDisabled("... %zu more: type to narrow the list", page.total - page.items.size());
		}
	}
	ImGui::EndChild();
	ImGui::EndPopup();
}
