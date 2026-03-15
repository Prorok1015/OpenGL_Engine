#include "edt_viewport_panel.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "res_tag.h"
#include "scn_camera_component.hpp"
#include "scn_model.h"
#include "../edt_guizmo.hpp"
#include <imgui.h>

namespace edt
{
	viewport_panel::viewport_panel()
		: panel_base("viewport", "Viewport")
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}



	void viewport_panel::set_registry(std::shared_ptr<entt::registry> registry)
	{
		m_registry = std::move(registry);
	}

	void viewport_panel::on_render()
	{
		render_toolbar();

		// Capture the exact image start position (after toolbar) for gizmo projection
		ImVec2 image_pos = ImGui::GetCursorScreenPos();
		ImVec2 size      = ImGui::GetContentRegionAvail();

		m_image_pos  = {image_pos.x, image_pos.y};
		m_viewport_size = {size.x, size.y};
		m_focused = ImGui::IsWindowFocused();

		// Update camera viewport so the renderer creates a correctly-sized render target
		glm::mat4 cam_view(1.f), cam_proj(1.f);
		if (m_registry && size.x > 0 && size.y > 0) {
			for (auto ent : m_registry->view<scn::camera_component>()) {
				auto& cam = m_registry->get<scn::camera_component>(ent);
				cam.m_viewport.size = glm::ivec2(static_cast<int>(size.x), static_cast<int>(size.y));

				if (m_registry->all_of<scn::local_transform>(ent))
					cam_view = glm::inverse(m_registry->get<scn::local_transform>(ent).local);
				float aspect = size.x / size.y;
				cam_proj = glm::perspective(
					glm::radians(cam.fov), aspect, cam.near_distance, cam.far_distance);
				break;
			}
		}

		// Block ECS input by default; allow only when cursor is over the viewport
		ImVec2 win_pos = ImGui::GetWindowPos();
		glm::ivec4 rect = {
			static_cast<int>(win_pos.x), static_cast<int>(win_pos.y),
			static_cast<int>(win_pos.x + size.x), static_cast<int>(win_pos.y + size.y)
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

			// Drop target: accept asset paths dragged from the Asset Browser
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
					std::string path(static_cast<const char*>(payload->Data),
					                 static_cast<size_t>(payload->DataSize) - 1);
					if (m_on_asset_dropped)
						m_on_asset_dropped(path);
				}
				ImGui::EndDragDropTarget();
			}
		} else {
			ImGui::TextDisabled("(no render target — load a level first)");
		}

		// Let ECS input through only when cursor is over the image
		if (ImGui::IsItemHovered() && m_ecs_input)
			m_ecs_input->set_input_area(rect);

		// Interactive transform gizmo (drawn on top of scene image)
		if (m_registry && m_selected != entt::null && m_registry->valid(m_selected))
			render_transform_gizmo(cam_view, cam_proj);

		render_orientation_gizmo();
		render_fps_overlay();
	}

	void viewport_panel::render_toolbar()
	{
		// Highlight the active gizmo operation button
		const char* labels[] = {"T", "R", "S"};
		for (int i = 0; i < 3; ++i) {
			bool active = (m_gizmo_op == i);
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if (ImGui::Button(labels[i]))
				m_gizmo_op = i;
			if (active)
				ImGui::PopStyleColor();
			ImGui::SameLine();
		}
		ImGui::TextDisabled("| FPS: %.1f", m_fps);
		ImGui::Separator();
	}

	void viewport_panel::render_orientation_gizmo()
	{
		if (!m_registry)
			return;

		// Find the first camera with a world transform to extract view/proj matrices
		glm::mat4 view(1.f);
		glm::mat4 proj(1.f);
		bool found_camera = false;

		for (auto ent : m_registry->view<scn::camera_component>()) {
			const auto& cam = m_registry->get<scn::camera_component>(ent);
			if (cam.m_viewport.size.x < 1 || cam.m_viewport.size.y < 1)
				continue;
			if (m_registry->all_of<scn::local_transform>(ent)) {
				const auto& lt = m_registry->get<scn::local_transform>(ent);
				view = glm::inverse(lt.local);
			}
			float aspect = static_cast<float>(cam.m_viewport.size.x) /
			               static_cast<float>(cam.m_viewport.size.y);
			proj = glm::perspective(glm::radians(cam.fov), aspect,
			                        cam.near_distance, cam.far_distance);
			found_camera = true;
			break;
		}

		if (!found_camera)
			return;

		// Draw orientation cube gizmo in the bottom-right corner of the scene image
		constexpr float gizmo_size = 80.f;
		float x = m_image_pos.x + m_viewport_size.x - gizmo_size - 8.f;
		float y = m_image_pos.y + m_viewport_size.y - gizmo_size - 8.f;

		edt::imgui::set_view_area(x, y, gizmo_size);
		edt::imgui::set_draw_list(ImGui::GetWindowDrawList());
		edt::imgui::draw_gizmo(view, proj);
	}

	void viewport_panel::render_transform_gizmo(const glm::mat4& view, const glm::mat4& proj)
	{
		edt::draw_transform_gizmo(
			*m_registry, m_selected,
			view, proj,
			m_image_pos.x, m_image_pos.y,
			m_viewport_size.x, m_viewport_size.y,
			m_gizmo_op);
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
