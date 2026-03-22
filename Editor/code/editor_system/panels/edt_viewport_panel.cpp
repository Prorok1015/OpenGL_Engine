#include "edt_viewport_panel.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "res_tag.h"
#include "scn_camera_component.hpp"
#include "scn_model.h"
#include "eng_transform_3d.hpp"
#include "edt_guizmo.hpp"
#include <imgui.h>

namespace edt
{
	static const res::tag s_editor_cam_tag = res::tag(res::tag::memory, "__editor_camera_rt");

	viewport_panel::viewport_panel(rnd::render_system& rnd, gui::gui_system& gui)
		: panel_base("viewport", "Viewport")
		, m_rnd(rnd)
		, m_gui(gui)
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}



	void viewport_panel::set_registry_getter(std::function<entt::registry*()> getter)
	{
		m_get_registry = std::move(getter);
	}

	void viewport_panel::on_render()
	{
		entt::registry* reg = m_get_registry ? m_get_registry() : nullptr;

		render_toolbar();

		// Capture the exact image start position (after toolbar) for gizmo projection
		ImVec2 image_pos = ImGui::GetCursorScreenPos();
		ImVec2 size      = ImGui::GetContentRegionAvail();

		m_image_pos  = {image_pos.x, image_pos.y};
		m_viewport_size = {size.x, size.y};
		m_focused = ImGui::IsWindowFocused();

		// Update camera viewport so the renderer creates a correctly-sized render target
		glm::mat4 cam_view(1.f), cam_proj(1.f);
		if (reg && size.x > 0 && size.y > 0) {
			for (auto ent : reg->view<scn::camera_component>()) {
				auto& cam = reg->get<scn::camera_component>(ent);
				if (cam.texture != s_editor_cam_tag) continue;
				cam.m_viewport.size = glm::ivec2(static_cast<int>(size.x), static_cast<int>(size.y));

				if (reg->all_of<scn::local_transform>(ent))
					cam_view = glm::inverse(reg->get<scn::local_transform>(ent).local);
				float aspect = size.x / size.y;
				cam_proj = glm::perspective(glm::radians(cam.fov), aspect, cam.near_distance, cam.far_distance);
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

		// Display the editor camera render target texture
		auto* texture = m_rnd.get_texture_manager().find(s_editor_cam_tag);
		if (texture) {
			auto* backend = m_gui.get_backend_interface();
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
		if (reg && m_selected != entt::null && reg->valid(m_selected))
			render_transform_gizmo(*reg, cam_view, cam_proj);

		// Re-fetch: on_transform_committed inside the gizmo may have triggered
		// reload_world(), which destroys the old scn::world and its registry.
		reg = m_get_registry ? m_get_registry() : nullptr;

		if (reg)
			render_orientation_gizmo(*reg);
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

		// Show Skeleton toggle — find skeleton_component on selected entity or its ancestors
		if (auto* reg = m_get_registry ? m_get_registry() : nullptr) {
			scn::skeleton_component* skel = find_skeleton_for_selected(*reg);
			if (skel) {
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
				bool active = skel->show_skeleton;
				if (active)
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				if (ImGui::Button("Bones"))
					skel->show_skeleton = !skel->show_skeleton;
				if (active)
					ImGui::PopStyleColor();
			}
		}

		ImGui::SameLine();
		ImGui::TextDisabled("| FPS: %.1f", m_fps);
		ImGui::Separator();
	}

	scn::skeleton_component* viewport_panel::find_skeleton_for_selected(entt::registry& reg)
	{
		if (m_selected == entt::null || !reg.valid(m_selected)) return nullptr;

		// Check selected entity itself
		if (auto* skel = reg.try_get<scn::skeleton_component>(m_selected))
			return skel;

		// Walk up ancestors
		entt::entity current = m_selected;
		while (auto* pc = reg.try_get<scn::parent_component>(current)) {
			current = pc->parent;
			if (auto* skel = reg.try_get<scn::skeleton_component>(current))
				return skel;
		}

		return nullptr;
	}

	void viewport_panel::render_orientation_gizmo(entt::registry& reg)
	{
		// Find the editor camera to extract view/proj matrices
		glm::mat4 view(1.f);
		glm::mat4 proj(1.f);
		bool found_camera = false;

		for (auto ent : reg.view<scn::camera_component>()) {
			const auto& cam = reg.get<scn::camera_component>(ent);
			if (cam.texture != s_editor_cam_tag) continue;
			if (cam.m_viewport.size.x < 1 || cam.m_viewport.size.y < 1)
				continue;
			if (reg.all_of<scn::local_transform>(ent)) {
				const auto& lt = reg.get<scn::local_transform>(ent);
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

	void viewport_panel::render_transform_gizmo(entt::registry& reg, const glm::mat4& view, const glm::mat4& proj)
	{
		bool committed = false;
		edt::draw_transform_gizmo(
			reg, m_selected,
			view, proj,
			m_image_pos.x, m_image_pos.y,
			m_viewport_size.x, m_viewport_size.y,
			m_gizmo_op, m_rotate_ent, m_active_ring, &committed);

		if (committed && m_on_transform_committed && reg.valid(m_selected)) {
			std::string name;
			if (reg.all_of<scn::name_component>(m_selected))
				name = reg.get<scn::name_component>(m_selected).name;

			if (!name.empty() && reg.all_of<scn::local_transform>(m_selected)) {
				const auto& lt = reg.get<scn::local_transform>(m_selected);
				eng::transform3d tr{ lt.local };
				m_on_transform_committed(name, tr.get_pos(), tr.get_angles_degrees(), tr.get_scale());
				// The callback may trigger reload_world() — `reg` is dangling from here on.
				return;
			}
		}
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
