#include "edt_prefab_editor_panel.h"
#include "edt_prefab_editor_context.h"
#include "edt_input_manager.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "inp_ecs_input_manager.h"
#include "inp_events.hpp"
#include "scn_camera_component.hpp"
#include "res_tag.h"
#include <imgui.h>
#include <entt/entt.hpp>

namespace edt
{
	static const res::tag s_prefab_cam_tag = res::tag{ prefab_editor_context::RT_COLOR };

	prefab_editor_panel::prefab_editor_panel(rnd::render_system& rnd, gui::gui_system& gui)
		: panel_base("prefab_editor", "Prefab Editor")
		, m_rnd(rnd)
		, m_gui(gui)
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		m_open = false; // hidden by default until a prefab is opened
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

		auto* texture = m_rnd.get_texture_manager().find(s_prefab_cam_tag);
		if (texture) {
			auto* backend = m_gui.get_backend_interface();
			ImTextureID tex_id = backend->get_imgui_texture_from_texture(texture);
			// UV (0,1)->(1,0) flips Y for OpenGL convention
			ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));

			// Route ECS input when cursor is over the prefab viewport
			if (ImGui::IsItemHovered()) {
				ImVec2 win_pos = ImGui::GetWindowPos();
				glm::ivec4 rect = {
					static_cast<int>(win_pos.x), static_cast<int>(win_pos.y),
					static_cast<int>(win_pos.x + size.x), static_cast<int>(win_pos.y + size.y)
				};
				if (m_ecs_input)
					m_ecs_input->set_input_area(rect);
				// Disable editor input blocking so mouse events reach ecs_input
				if (m_editor_input)
					m_editor_input->set_input_area(glm::zero<glm::ivec4>(), true);
			}

			// Gate per-world camera input: only active when this viewport is hovered
			if (entt::registry* reg = m_ctx->get_registry()) {
				auto& focus = reg->ctx().emplace<inp::input_focus>();
				focus.active = ImGui::IsItemHovered();
			}
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
			cam.m_viewport.size = glm::ivec2(static_cast<int>(width), static_cast<int>(height));
			break;
		}
	}
}
