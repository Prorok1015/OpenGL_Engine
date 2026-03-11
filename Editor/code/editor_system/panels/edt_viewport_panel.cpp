#include "edt_viewport_panel.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "res_tag.h"
#include "scn_camera_component.hpp"
#include <imgui.h>

namespace edt
{
	viewport_panel::viewport_panel()
		: panel_base("viewport", "Viewport")
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}

	void viewport_panel::set_renderer(std::shared_ptr<scn::renderer_3d> renderer)
	{
		m_renderer = std::move(renderer);
	}

	void viewport_panel::set_registry(std::shared_ptr<entt::registry> registry)
	{
		m_registry = std::move(registry);
	}

	void viewport_panel::on_render()
	{
		render_toolbar();

		ImVec2 pos  = ImGui::GetWindowPos();
		ImVec2 size = ImGui::GetContentRegionAvail();
		m_viewport_pos  = {pos.x, pos.y};
		m_viewport_size = {size.x, size.y};
		m_focused = ImGui::IsWindowFocused();

		// Update camera viewport so the renderer creates a correctly-sized render target
		if (m_registry && size.x > 0 && size.y > 0) {
			for (auto ent : m_registry->view<scn::camera_component>()) {
				auto& cam = m_registry->get<scn::camera_component>(ent);
				cam.m_viewport.size = glm::ivec2(static_cast<int>(size.x), static_cast<int>(size.y));
			}
		}

		// Block ECS input by default; allow only when cursor is over the viewport
		glm::ivec4 rect = {
			static_cast<int>(pos.x), static_cast<int>(pos.y),
			static_cast<int>(pos.x + size.x), static_cast<int>(pos.y + size.y)
		};
		if (m_ecs_input)
			m_ecs_input->set_input_area(glm::zero<glm::ivec4>(), true);
		if (m_input)
			m_input->set_input_area(rect, true);

		// Display the scene render target texture
		static const res::tag color_rt_tag = res::tag(res::tag::memory, "__color_scene_rt");
		auto* texture = rnd::get_system().get_texture_manager().find(color_rt_tag);
		if (texture) {
			auto* backend = gui::get_system().get_backend_interface();
			ImTextureID tex_id = backend->get_imgui_texture_from_texture(texture);
			// UV (0,1)→(1,0) flips Y for OpenGL convention
			ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));
		} else {
			ImGui::TextDisabled("(no render target — load a level first)");
		}

		// Let ECS input through only when cursor is over the image
		if (ImGui::IsItemHovered() && m_ecs_input)
			m_ecs_input->set_input_area(rect);

		render_fps_overlay();
	}

	void viewport_panel::render_toolbar()
	{
		if (ImGui::Button("T")) m_gizmo_op = 0;
		ImGui::SameLine();
		if (ImGui::Button("R")) m_gizmo_op = 1;
		ImGui::SameLine();
		if (ImGui::Button("S")) m_gizmo_op = 2;
		ImGui::SameLine();
		ImGui::TextDisabled("| FPS: %.1f", m_fps);
		ImGui::Separator();
	}

	void viewport_panel::render_fps_overlay()
	{
		float dt = ImGui::GetIO().DeltaTime;
		m_frame_count++;
		m_fps_timer += dt;
		if (m_fps_timer >= 1.f) {
			m_fps = static_cast<float>(m_frame_count) / m_fps_timer;
			m_frame_count = 0;
			m_fps_timer = 0.f;
		}
	}
}
