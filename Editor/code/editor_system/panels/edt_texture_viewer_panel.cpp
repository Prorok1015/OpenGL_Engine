#include "edt_texture_viewer_panel.h"
#include "edt_texture_window_panel.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_theme.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <imgui.h>
#include <filesystem>
#include <format>

namespace
{
	std::string viewer_title(const res::tag& source, const std::string& id)
	{
		const std::string name = std::filesystem::path(std::string(source.relative())).filename().generic_string();
		return std::format("{} (read-only)###{}", name, id);
	}
}

edt::texture_viewer_panel::texture_viewer_panel(std::string id, res::tag source, texture_preview_source preview, texture_source shown,
	int mip_levels, editor_services& services, preview_view view)
	: panel_base(id, viewer_title(source, id))
	, m_source(std::move(source))
	, m_shown(std::move(shown))
	, m_mip_levels(mip_levels)
	, m_render(view.render)
	, m_content(std::move(preview), services)
	, m_stage(services, m_content, view)
{
}

edt::texture_viewer_panel::~texture_viewer_panel()
{
	// The world and the copies go first: nothing samples the texture or reloads it once it is removed.
	m_stage.close();
	if (m_render) m_render->get_texture_manager().remove(m_content.texture_tag());
}

void edt::texture_viewer_panel::set_create(std::function<void()> create, std::string refusal)
{
	m_create = std::move(create);
	m_refusal = std::move(refusal);
}

void edt::texture_viewer_panel::on_before_begin()
{
	ImGui::SetNextWindowSize(ImVec2(em(420.f), em(380.f)), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::texture_viewer_panel::on_hidden()
{
	m_stage.park();
	if (!is_open() && m_on_close) m_on_close();
}

void edt::texture_viewer_panel::on_render()
{
	m_stage.update();
	ImGui::TextDisabled("%s", std::string(m_source.view()).c_str());
	if (m_open_existing) {
		// A copy: the owner sets the buttons again from inside the call.
		if (ImGui::Button("Open Texture")) {
			const std::function<void()> open = m_open_existing;
			open();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The texture desc beside the picture names it: open it in the texture window.");
		}
	} else if (m_create) {
		ImGui::BeginDisabled(!m_refusal.empty());
		if (ImGui::Button("Create Texture")) {
			const std::function<void()> create = m_create;
			create();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", m_refusal.empty() ? "Write a texture desc beside the picture and open it in the texture window."
				: m_refusal.c_str());
		}
		if (!m_error.empty()) {
			ImGui::TextColored(colors().error, "%s", m_error.c_str());
		}
	} else {
		ImGui::TextDisabled("Read-only: the file is not in the project's own layer.");
	}
	draw_texture_preview(m_stage, m_render, m_content.texture_tag(), m_shown, m_mip_levels);
}
