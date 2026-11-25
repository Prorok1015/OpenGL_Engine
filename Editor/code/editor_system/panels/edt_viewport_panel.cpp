#include "edt_viewport_panel.h"
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/gui/gui_system.h>
#include <snk/gui/gui_theme.h>
#include <snk/gui/gui_ui_font.h>
#include <snk/model_codec/res_tag.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/input/inp_events.hpp>
#include "edt_guizmo.hpp"
#include "edt_level_disk_banner.h"
#include "edt_editor_camera.h"
#include "edt_ui_probe_imgui.h"
#include "edt_ui_metrics.h"
#include "edt_viewport_picker.h"
#include <imgui.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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



	const res::tag& viewport_panel::render_target_tag()
	{
		return s_editor_cam_tag;
	}

	void viewport_panel::on_hidden()
	{
		const ui_probe::scoped_pass probe_pass(m_ui_probe.get(), {"viewport_toolbar"});
		if (m_click) m_click->cancel();
		if (entt::registry* reg = m_get_registry ? m_get_registry() : nullptr)
			set_editor_camera_input(*reg, false);
	}

	void viewport_panel::suspend_camera()
	{
		entt::registry* reg = m_get_registry ? m_get_registry() : nullptr;
		if (!reg) return;
		set_editor_camera_input(*reg, false);
		for (auto ent : reg->view<scn::camera_component>()) {
			auto& cam = reg->get<scn::camera_component>(ent);
			if (cam.color_targets.empty() || cam.color_targets[0] != s_editor_cam_tag) continue;
			if (cam.m_viewport.size != glm::ivec2{ 0 }) m_suspended_size = cam.m_viewport.size;
			cam.m_viewport.size = glm::ivec2{ 0 };
		}
	}

	void viewport_panel::resume_camera()
	{
		const std::optional<glm::ivec2> size = std::exchange(m_suspended_size, std::nullopt);
		entt::registry* reg = m_get_registry ? m_get_registry() : nullptr;
		if (!reg || !size) return;
		// A visible panel sizes it again in its next on_render anyway; this covers a closed or hidden one.
		for (auto ent : reg->view<scn::camera_component>()) {
			auto& cam = reg->get<scn::camera_component>(ent);
			if (cam.color_targets.empty() || cam.color_targets[0] != s_editor_cam_tag) continue;
			if (cam.m_viewport.size == glm::ivec2{ 0 }) cam.m_viewport.size = *size;
		}
	}

	void viewport_panel::render_disk_banner()
	{
		if (!m_disk_banner || !m_disk_banner->visible()) return;
		const std::string_view text = level_disk_banner::k_text;
		ImGui::TextColored(colors().warning, "%.*s", static_cast<int>(text.size()), text.data());
		// The actions are queued: the level is rebuilt after the frame, never under the registry this panel draws.
		const std::optional<std::string> blocked = m_disk_banner->blocked_reason();
		const auto explain = [&blocked] {
			if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", blocked->c_str());
		};
		ImGui::SameLine();
		ImGui::BeginDisabled(blocked.has_value());
		if (ImGui::SmallButton("Reload##level_disk")) m_disk_banner->reload();
		explain();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::SmallButton("Keep mine##level_disk")) m_disk_banner->keep_mine();
		ImGui::SameLine();
		ImGui::BeginDisabled(blocked.has_value());
		if (ImGui::SmallButton("Overwrite##level_disk")) m_disk_banner->overwrite();
		explain();
		ImGui::EndDisabled();
		if (const std::string& error = m_disk_banner->error(); !error.empty()) {
			ImGui::TextColored(colors().error, "%s", error.c_str());
		}
	}

	void viewport_panel::set_registry_getter(std::function<entt::registry*()> getter)
	{
		m_get_registry = std::move(getter);
	}

	void viewport_panel::on_render()
	{
		const ui_probe::scoped_pass probe_pass(m_ui_probe.get(), {"viewport_toolbar"});
		entt::registry* reg = m_get_registry ? m_get_registry() : nullptr;
		// A registry that is not the one seen last time (the world was rebuilt): the selected entity is taken again and a
		// drag of the rotate ring that was in progress ends.
		const bool world_replaced = m_selection.refresh(reg);
		if (world_replaced) m_rotate.reset();
		const bool locked = m_edits_locked && m_edits_locked();

		render_toolbar();
		if (locked) {
			ImGui::SameLine();
			ImGui::TextDisabled("Playing: stop Play to edit");
		}
		render_disk_banner();

		// Capture the exact image start position (after toolbar) for gizmo projection
		ImVec2 image_pos = ImGui::GetCursorScreenPos();
		ImVec2 size      = ImGui::GetContentRegionAvail();

		m_image_pos  = {image_pos.x, image_pos.y};
		m_viewport_size = {size.x, size.y};
		m_focused = ImGui::IsWindowFocused();

		// Update camera viewport so the renderer creates a correctly-sized render target
		glm::mat4 cam_view(1.f), cam_proj(1.f);
		bool have_camera = false;
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		if (reg && size.x > 0 && size.y > 0) {
			for (auto ent : reg->view<scn::camera_component>()) {
				auto& cam = reg->get<scn::camera_component>(ent);
				if (cam.color_targets.empty() || cam.color_targets[0] != s_editor_cam_tag) continue;
				cam.m_viewport.size = edt::to_framebuffer_size({ size.x, size.y }, { fb_scale.x, fb_scale.y });
				cam.wireframe = m_wireframe;

				if (reg->all_of<scn::local_transform>(ent))
					cam_view = glm::inverse(reg->get<scn::local_transform>(ent).local);
				float aspect = size.x / size.y;
				cam_proj = glm::perspective(glm::radians(cam.fov), aspect, cam.near_distance, cam.far_distance);
				have_camera = true;
				break;
			}
		}

		// Display the editor camera render target texture
		auto& textures = m_rnd.get_texture_manager();
		if (const auto texture = textures.binding(textures.id_of(s_editor_cam_tag)).handle) {
			auto* backend = m_gui.get_backend_interface();
			ImTextureID tex_id = backend->to_imgui(texture);
			// UV (0,1)→(1,0) flips Y for OpenGL convention
			ImGui::Image(tex_id, size, ImVec2(0, 1), ImVec2(1, 0));

			// Drop target: accept asset paths dragged from the Asset Browser
			if (!locked && ImGui::BeginDragDropTarget()) {
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

		const view_item view = capture_view_item();

		// Interactive transform gizmo (drawn on top of scene image)
		if (!locked && reg && m_selection.get() != entt::null && reg->valid(m_selection.get()))
			render_transform_gizmo(*reg, cam_view, cam_proj);

		// Re-fetch: on_transform_committed inside the gizmo may have triggered
		// reload_world(), which destroys the old scn::world_context and its registry.
		entt::registry* const shown_reg = reg;
		reg = m_get_registry ? m_get_registry() : nullptr;
		const bool same_world = !world_replaced && reg == shown_reg;

		// Input (and the camera focus derived from it) reaches the world only while the image is hovered and no gizmo handle
		// is in the way; the editor camera reads it only then (the Game view shares the world and must not move it, #545).
		std::optional<inp::input_target> target;
		if (m_input_target)
			target = m_input_target();
		const bool takes_input = bind_view_input(m_router, target, view, is_dragging_gizmo());
		if (reg)
			run_click(*reg, takes_input && !locked, have_camera && same_world, locked, cam_view, cam_proj);
		else if (m_click)
			m_click->cancel();
		reg = m_get_registry ? m_get_registry() : nullptr;
		if (reg)
			set_editor_camera_input(*reg, takes_input && (!m_click || m_click->camera_input()));

		if (reg)
			render_orientation_gizmo(*reg);
		render_fps_overlay();
	}

	void viewport_panel::run_click(entt::registry& reg, bool takes_input, bool usable, bool locked, const glm::mat4& cam_view, const glm::mat4& cam_proj)
	{
		if (!m_click)
			m_click.emplace(dpi_px(4.f));
		const ImGuiIO& io = ImGui::GetIO();
		const ImVec2 mouse = io.MousePos;
		const glm::vec2 pixel{ mouse.x - m_image_pos.x, mouse.y - m_image_pos.y };
		if (!usable) {
			m_click->cancel();
			return;
		}
		const mesh::gesture_result result = m_click->update({
			.takes_input = takes_input,
			.locked = locked,
			.pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left),
			.down = ImGui::IsMouseDown(ImGuiMouseButton_Left),
			.released = ImGui::IsMouseReleased(ImGuiMouseButton_Left),
			.interrupted = !m_focused,
			.pixel = pixel,
			.modifiers = { io.KeyShift, io.KeyCtrl, io.KeyAlt },
		});
		if (result.kind != mesh::gesture_end::click || !m_on_pick)
			return;
		const auto picked = pick_node(reg, { cam_view, cam_proj, result.point, m_viewport_size });
		if (picked)
			m_on_pick(picked->path);
		else
			m_on_pick(std::nullopt);
	}

	void viewport_panel::render_toolbar()
	{
		// Highlight the active gizmo operation button
		const char* labels[] = {"T", "R", "S"};
		const char* icons[] = {gui::icon::move, gui::icon::rotate, gui::icon::scale};
		const bool with_icons = gui::ui_font_has_icons();
		for (int i = 0; i < 3; ++i) {
			bool active = (m_gizmo_op == i);
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			// The same ID with or without the icon; the probe keeps the letter as the name.
			const std::string button = std::format("{}###{}", with_icons ? icons[i] : labels[i], labels[i]);
			if (ImGui::Button(button.c_str()))
				m_gizmo_op = i;
			probe_last_item(m_ui_probe.get(), "viewport_toolbar", labels[i]);
			if (active)
				ImGui::PopStyleColor();
			ImGui::SameLine();
		}

		ImGui::TextDisabled("|");
		ImGui::SameLine();
		{
			const bool active = m_wireframe;
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if (ImGui::Button("Wireframe"))
				m_wireframe = !m_wireframe;
			probe_last_item(m_ui_probe.get(), "viewport_toolbar", "Wireframe");
			if (active)
				ImGui::PopStyleColor();
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
				probe_last_item(m_ui_probe.get(), "viewport_toolbar", "Bones");
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
		if (m_selection.get() == entt::null || !reg.valid(m_selection.get())) return nullptr;

		// Check selected entity itself
		if (auto* skel = reg.try_get<scn::skeleton_component>(m_selection.get()))
			return skel;

		// Walk up ancestors
		entt::entity current = m_selection.get();
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
			if (cam.color_targets.empty() || cam.color_targets[0] != s_editor_cam_tag) continue;
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
		const float gizmo_size = em(80.f);
		float x = m_image_pos.x + m_viewport_size.x - gizmo_size - em(8.f);
		float y = m_image_pos.y + m_viewport_size.y - gizmo_size - em(8.f);

		edt::imgui::set_view_area(x, y, gizmo_size);
		edt::imgui::set_draw_list(ImGui::GetWindowDrawList());
		edt::imgui::draw_gizmo(view, proj);
	}

	void viewport_panel::render_transform_gizmo(entt::registry& reg, const glm::mat4& view, const glm::mat4& proj)
	{
		bool committed = false;
		edt::draw_transform_gizmo(
			reg, m_selection.get(),
			view, proj,
			m_image_pos.x, m_image_pos.y,
			m_viewport_size.x, m_viewport_size.y,
			m_gizmo_op, m_rotate, &committed);

		if (committed && m_on_transform_committed && reg.valid(m_selection.get())) {
			// Copied: the callback may reload the world and with it the component this path lives in.
			std::vector<uint64_t> path;
			if (const auto* id = reg.try_get<scn::node_id_component>(m_selection.get()))
				path = id->path;

			if (!path.empty() && reg.all_of<scn::local_transform>(m_selection.get())) {
				const auto& lt = reg.get<scn::local_transform>(m_selection.get());
				eng::transform3d tr{ lt.local };
				m_on_transform_committed(path, tr.get_pos(), tr.get_angles_degrees(), tr.get_scale());
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
