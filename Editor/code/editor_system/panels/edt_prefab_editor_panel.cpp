#include "edt_prefab_editor_panel.h"
#include "edt_prefab_document.h"
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

	void prefab_editor_panel::on_before_begin()
	{
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
		// Apply and Revert buttons — disabled in Phase 1
		ImGui::BeginDisabled(true);
		ImGui::Button("Apply");
		ImGui::SameLine();
		ImGui::Button("Revert");
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		if (ImGui::Button("Close")) {
			if (m_on_close) {
				m_on_close();
			}
		}

		ImGui::Separator();
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
			// UV (0,1)->(1,0) flips Y for OpenGL convention
			ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));

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
