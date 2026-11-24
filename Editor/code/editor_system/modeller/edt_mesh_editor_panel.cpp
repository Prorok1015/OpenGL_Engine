#include "edt_mesh_editor_panel.h"
#include "edt_input_manager.h"
#include "edt_mesh_document.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_system.h>
#include <snk/gui/gui_theme.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <cfloat>
#include <imgui.h>

namespace edt
{
	mesh_editor_panel::mesh_editor_panel(rnd::render_system& rnd, gui::gui_system& gui)
		: panel_base("mesh_editor", "Mesh Modeller")
		, m_rnd(rnd)
		, m_gui(gui)
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		m_open = false;
	}

	void mesh_editor_panel::set_context(mesh_document* shown)
	{
		if (m_ctx && m_ctx != shown) m_ctx->park_camera();
		m_ctx = shown;
		m_error.clear();
	}

	void mesh_editor_panel::on_hidden()
	{
		if (!is_open() && m_ctx && m_on_close) m_on_close();
	}

	void mesh_editor_panel::on_before_begin()
	{
		m_title = (m_ctx && m_ctx->is_open() ? "Mesh: " + m_ctx->display_title() : std::string{ "Mesh Modeller" }) + "###mesh_editor";
		ImGui::SetNextWindowSize(ImVec2(em(800.f), em(600.f)), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(em(420.f), em(300.f)), ImVec2(FLT_MAX, FLT_MAX));
	}

	void mesh_editor_panel::on_render()
	{
		if (!m_ctx || !m_ctx->is_open()) {
			ImGui::TextDisabled("(no mesh open)");
			return;
		}
		render_toolbar();
		if (!m_ctx || !m_ctx->is_open()) return;
		render_banners();
		ImGui::Separator();
		render_viewport();
	}

	void mesh_editor_panel::render_toolbar()
	{
		const bool locked = m_ctx->edits_locked();
		ImGui::BeginDisabled(locked || !m_ctx->is_dirty());
		if (ImGui::Button("Save") && m_on_save) m_on_save();
		ImGui::EndDisabled();
		if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Stop Play to save the mesh");
		ImGui::SameLine();
		ImGui::BeginDisabled(locked || !m_ctx->is_dirty());
		if (ImGui::Button("Revert") && m_on_revert) m_on_revert();
		ImGui::EndDisabled();
		if (m_ctx->geometry_mismatch() || m_ctx->geometry_stale()) {
			ImGui::SameLine();
			ImGui::BeginDisabled(locked);
			if (ImGui::Button("Overwrite geometry") && m_on_overwrite_geometry) m_on_overwrite_geometry();
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("Writes the geometry file from the source, replacing the one on disk");
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		if (ImGui::Button("Close")) press_close();
		// A clean mesh closes at once and takes the document with it: nothing of it may be read after that.
		if (!m_ctx || !m_ctx->is_open()) return;
		const mesh_stats& stats = m_ctx->stats();
		ImGui::SameLine();
		ImGui::TextDisabled("%zu vertices, %zu faces, %zu triangles", stats.vertices, stats.faces, stats.triangles);
	}

	void mesh_editor_panel::render_banners()
	{
		if (m_ctx->changed_on_disk()) {
			ImGui::TextColored(colors().warning, "The file changed on disk. Reload drops your edits, Overwrite replaces the file with them.");
			if (ImGui::Button("Reload")) m_error = m_ctx->reload();
			ImGui::SameLine();
			if (ImGui::Button("Overwrite")) {
				const save_result saved = m_ctx->overwrite();
				m_error = saved.saved ? std::string{} : saved.error;
			}
		}
		if (!m_ctx->preview_problem().empty()) {
			ImGui::TextColored(colors().warning, "%s", m_ctx->preview_problem().c_str());
		}
		if (m_ctx->geometry_stale()) {
			ImGui::TextColored(colors().error, "%s", m_ctx->stale_geometry_message().c_str());
		} else if (m_ctx->geometry_mismatch()) {
			ImGui::TextColored(colors().warning, "%s", m_ctx->geometry_mismatch_message().c_str());
		}
		if (!m_error.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, colors().error);
			ImGui::TextWrapped("%s", m_error.c_str());
			ImGui::PopStyleColor();
		}
	}

	void mesh_editor_panel::render_viewport()
	{
		const ImVec2 size = ImGui::GetContentRegionAvail();
		if (size.x <= 0 || size.y <= 0) return;
		scn::camera_component* camera = m_ctx->camera();
		if (!camera) {
			ImGui::TextDisabled("(no preview)");
			return;
		}
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		camera->m_viewport.size = edt::to_framebuffer_size({ size.x, size.y }, { fb_scale.x, fb_scale.y });

		auto& textures = m_rnd.get_texture_manager();
		const res::tag target{ m_ctx->render_target() };
		if (const auto texture = textures.binding(textures.id_of(target)).handle) {
			ImTextureID tex_id = m_gui.get_backend_interface()->to_imgui(texture);
			const ImVec2 image_pos = ImGui::GetCursorScreenPos();
			ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));
			ImGui::SetCursorScreenPos(image_pos);
			ImGui::InvisibleButton("##mesh_preview_input", size);
			bind_item_input(m_router, inp::input_target{ .set = m_ctx->set_name(), .context = std::string{ mesh_document::context_name() } });
		} else {
			ImGui::TextDisabled("(waiting for render target...)");
		}
	}
}
