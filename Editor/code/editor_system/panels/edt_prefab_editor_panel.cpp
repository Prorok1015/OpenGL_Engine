#include "edt_prefab_editor_panel.h"
#include "edt_prefab_document.h"
#include "edt_prefab_close.h"
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/gui/gui_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/resource/res_tag.h>
#include <cfloat>
#include <imgui.h>
#include <entt/entt.hpp>

namespace edt
{
	static const res::tag s_prefab_cam_tag = res::tag{ prefab_document::RT_COLOR };

	prefab_editor_panel::prefab_editor_panel(rnd::render_system& rnd, gui::gui_system& gui)
		: panel_base("prefab_editor", "Prefab Editor")
		, m_rnd(rnd)
		, m_gui(gui)
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		m_open = false; // hidden by default until a prefab is opened
	}

	void prefab_editor_panel::on_hidden()
	{
		// Collapsed or behind another tab the window is still open: only a closed one lets the document go.
		if (!is_open() && m_ctx && m_on_close) {
			m_on_close();
		}
	}

	void prefab_editor_panel::on_before_begin()
	{
		// The unapplied-edits mark is in the window's title; ### keeps its ImGui id while the text changes.
		m_title = (m_ctx && m_ctx->is_open() ? "Prefab: " + m_ctx->display_title() : std::string{ "Prefab Editor" }) + "###prefab_editor";
		ImGui::SetNextWindowSize(ImVec2(900.f, 650.f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(480.f, 320.f), ImVec2(FLT_MAX, FLT_MAX));
	}

	void prefab_editor_panel::on_render()
	{
		if (!m_ctx || !m_ctx->is_open()) {
			ImGui::TextDisabled("(no prefab open)");
			// If context was closed externally, close the panel
			if (m_open && m_ctx && !m_ctx->is_open()) {
				m_open = false;
			}
			return;
		}

		render_toolbar();

		// Close callback in toolbar may have nulled the context
		if (!m_ctx || !m_ctx->is_open()) return;

		render_viewport();
	}

	void prefab_editor_panel::render_toolbar()
	{
		const prefab_toolbar_state state = toolbar_state_of(*m_ctx);
		ImGui::BeginDisabled(!state.apply_enabled);
		if (ImGui::Button("Apply") && m_on_apply) {
			m_on_apply();
		}
		ImGui::EndDisabled();
		if (!state.apply_hint.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", state.apply_hint.c_str());
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!state.revert_enabled);
		if (ImGui::Button("Revert") && m_on_revert) {
			m_on_revert();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		if (ImGui::Button("Close")) {
			if (m_on_close) {
				m_on_close();
			}
		}

		render_disk_banner();
		ImGui::Separator();
	}

	void prefab_editor_panel::render_disk_banner()
	{
		if (!m_ctx || !m_ctx->changed_on_disk()) {
			m_banner_later = false;
			return;
		}
		if (m_banner_later) {
			// Later keeps a compact line, so the way out stays one click away.
			ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "Changed on disk");
			ImGui::SameLine();
			if (ImGui::SmallButton("Reload")) {
				m_error = m_ctx->reload();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Overwrite")) {
				if (const save_result saved = m_ctx->overwrite(); !saved.saved) {
					m_error = saved.error;
				} else {
					m_error.clear();
				}
			}
			if (!m_error.empty()) {
				ImGui::TextWrapped("%s", m_error.c_str());
			}
			return;
		}
		ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "The file changed on disk. Reload drops your edits, Overwrite replaces the file with them.");
		if (ImGui::Button("Reload")) {
			m_error = m_ctx->reload();
		}
		ImGui::SameLine();
		if (ImGui::Button("Overwrite")) {
			if (const save_result saved = m_ctx->overwrite(); !saved.saved) {
				m_error = saved.error;
			} else {
				m_error.clear();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Later")) {
			m_banner_later = true;
			m_error.clear();
		}
		if (!m_error.empty()) {
			ImGui::TextWrapped("%s", m_error.c_str());
		}
	}

	void prefab_editor_panel::render_viewport()
	{
		ImVec2 size = ImGui::GetContentRegionAvail();
		if (size.x <= 0 || size.y <= 0) return;

		update_camera_viewport(size.x, size.y);

		auto& textures = m_rnd.get_texture_manager();
		if (const auto texture = textures.binding(textures.id_of(s_prefab_cam_tag)).handle) {
			auto* backend = m_gui.get_backend_interface();
			ImTextureID tex_id = backend->to_imgui(texture);
			const ImVec2 image_pos = ImGui::GetCursorScreenPos();
			// UV (0,1)->(1,0) flips Y for OpenGL convention
			ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));
			// Give the preview drag an active item so ImGui does not also move the floating window.
			ImGui::SetCursorScreenPos(image_pos);
			ImGui::InvisibleButton("##prefab_preview_input", size);

			// The preview context gets input only while its image is hovered
			bind_item_input(m_router, inp::input_target{
				.set = m_ctx->set_name(),
				.context = std::string{ prefab_document::context_name() } });
		} else {
			ImGui::TextDisabled("(waiting for render target...)");
		}
	}

	void prefab_editor_panel::update_camera_viewport(float width, float height)
	{
		entt::registry* reg = m_ctx ? m_ctx->get_registry() : nullptr;
		if (!reg) return;

		for (auto ent : reg->view<scn::camera_component>()) {
			auto& cam = reg->get<scn::camera_component>(ent);
			if (cam.color_targets.empty() || cam.color_targets[0] != s_prefab_cam_tag) continue;
			const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
			cam.m_viewport.size = edt::to_framebuffer_size({ width, height }, { fb_scale.x, fb_scale.y });
			break;
		}
	}
}
