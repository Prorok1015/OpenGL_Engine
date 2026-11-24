#include "edt_mesh_editor_panel.h"
#include "edt_input_manager.h"
#include "edt_mesh_document.h"
#include "edt_mesh_overlay.h"
#include "edt_ui_metrics.h"
#include <snk/gui/gui_system.h>
#include <snk/gui/gui_theme.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <cfloat>
#include <optional>
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

	void mesh_editor_panel::restore_camera_input()
	{
		if (!m_ctx) return;
		if (scn::mouse_controller_component* controller = m_ctx->camera_controller()) controller->input_enabled = true;
	}

	void mesh_editor_panel::release_camera()
	{
		m_gesture.cancel();
		m_omitted_segments = 0;
		m_hover_key.reset();
		restore_camera_input();
	}

	void mesh_editor_panel::set_context(mesh_document* shown)
	{
		if (m_ctx && m_ctx != shown) {
			release_camera();
			m_ctx->park_camera();
		}
		m_ctx = shown;
		m_error.clear();
		m_omitted_segments = 0;
		m_hover_key.reset();
	}

	void mesh_editor_panel::on_hidden()
	{
		release_camera();
		if (!is_open() && m_ctx && m_on_close) m_on_close();
	}

	void mesh_editor_panel::on_before_begin()
	{
		// A frame in which the window is not drawn (collapsed, in a hidden tab) never reaches on_render: the camera must not stay locked.
		restore_camera_input();
		m_title = (m_ctx && m_ctx->is_open() ? "Mesh: " + m_ctx->display_title() : std::string{ "Mesh Modeller" }) + "###mesh_editor";
		ImGui::SetNextWindowSize(ImVec2(em(800.f), em(600.f)), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(em(420.f), em(300.f)), ImVec2(FLT_MAX, FLT_MAX));
	}

	void mesh_editor_panel::on_render()
	{
		if (!m_ctx || !m_ctx->is_open()) {
			release_camera();
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
		render_mode_bar();
	}

	void mesh_editor_panel::render_mode_bar()
	{
		const mesh::poly_mesh* working = m_ctx->working();
		if (!working) return;
		mesh::mesh_view& view = m_ctx->view();
		handle_mode_keys();
		const mesh::select_mode mode = view.mode();
		const auto mode_button = [&](const char* label, mesh::select_mode value) {
			if (ImGui::RadioButton(label, mode == value) && mode != value) view.set_mode(value, *working);
			ImGui::SameLine();
		};
		mode_button("Vertex", mesh::select_mode::vertex);
		mode_button("Edge", mesh::select_mode::edge);
		mode_button("Face", mesh::select_mode::face);
		bool xray = view.xray();
		if (ImGui::Checkbox("X-ray", &xray)) view.set_xray(xray);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show what faces away from the camera (Alt+Z)");
		ImGui::SameLine();
		ImGui::TextDisabled("%zu selected", view.current_selection(*working).size());
		if (m_omitted_segments > 0) {
			ImGui::SameLine();
			ImGui::TextColored(colors().warning, "%zu edges not drawn", m_omitted_segments);
		}
	}

	void mesh_editor_panel::handle_mode_keys()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput || m_gesture.active() || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
		const mesh::poly_mesh* working = m_ctx->working();
		mesh::mesh_view& view = m_ctx->view();
		const bool no_modifier = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
		if (no_modifier) {
			if (ImGui::IsKeyPressed(ImGuiKey_1, false)) view.set_mode(mesh::select_mode::vertex, *working);
			if (ImGui::IsKeyPressed(ImGuiKey_2, false)) view.set_mode(mesh::select_mode::edge, *working);
			if (ImGui::IsKeyPressed(ImGuiKey_3, false)) view.set_mode(mesh::select_mode::face, *working);
		}
		const bool alt_alone = io.KeyAlt && !io.KeyCtrl && !io.KeyShift && !io.KeySuper;
		if (alt_alone && ImGui::IsKeyPressed(ImGuiKey_Z, false)) view.set_xray(!view.xray());
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
		if (size.x <= 0 || size.y <= 0) {
			release_camera();
			return;
		}
		scn::camera_component* camera = m_ctx->camera();
		if (!camera) {
			release_camera();
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
			const bool hovered = ImGui::IsItemHovered();
			bind_item_input(m_router, inp::input_target{ .set = m_ctx->set_name(), .context = std::string{ mesh_document::context_name() } });
			const std::optional<glm::mat4> matrix = m_ctx->view_projection();
			if (matrix && m_ctx->working()) {
				const mesh::screen_view screen(*matrix, { image_pos.x, image_pos.y }, { size.x, size.y });
				interact_with_image(screen, hovered);
				draw_overlay(screen, image_pos, size, hovered);
			} else {
				release_camera();
			}
		} else {
			release_camera();
			ImGui::TextDisabled("(waiting for render target...)");
		}
	}

	void mesh_editor_panel::interact_with_image(const mesh::screen_view& screen, bool hovered)
	{
		const mesh::poly_mesh& working = *m_ctx->working();
		mesh::mesh_view& view = m_ctx->view();
		const ImGuiIO& io = ImGui::GetIO();
		const glm::vec2 mouse{ io.MousePos.x, io.MousePos.y };
		const float pick_radius = em(6.f);
		const mesh::pick_query query = view.query(working, screen);

		m_gesture.set_drag_threshold(em(4.f));
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			m_gesture.begin(mouse, mesh::key_modifiers{ .shift = io.KeyShift, .ctrl = io.KeyCtrl, .alt = io.KeyAlt });
		}
		if (m_gesture.active()) m_gesture.move(mouse);
		if (m_gesture.active() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			const mesh::gesture_result result = m_gesture.end(mouse);
			mesh::mesh_selection& selection = view.current_selection(working);
			if (result.kind == mesh::gesture_end::click) {
				mesh::click_select(selection, query, result.point, pick_radius, result.op);
			} else if (result.kind == mesh::gesture_end::box) {
				mesh::box_select(selection, query, result.rect, result.op);
			}
		} else {
			m_gesture.release_lost(ImGui::IsMouseDown(ImGuiMouseButton_Left));
		}
		if (scn::mouse_controller_component* controller = m_ctx->camera_controller()) controller->input_enabled = m_gesture.camera_input();

		const bool pointing = hovered && !m_gesture.active() && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
		const hover_key key{ mouse, screen.view_projection(), screen.origin(), screen.size(), working.generation(), view.mode(), pointing };
		if (m_hover_key != key) {
			m_hover_key = key;
			mesh::hover_target hover;
			if (pointing) {
				switch (view.mode()) {
				case mesh::select_mode::vertex:
					hover.vertex = mesh::pick_vertex(query, mouse, pick_radius);
					break;
				case mesh::select_mode::edge:
					hover.edge = mesh::pick_edge(query, mouse, pick_radius);
					break;
				case mesh::select_mode::face:
					hover.face = mesh::pick_face(query, mouse);
					break;
				}
			}
			view.set_hover(hover);
		}
	}

	void mesh_editor_panel::draw_overlay(const mesh::screen_view& screen, const ImVec2& image_pos, const ImVec2& image_size, bool hovered)
	{
		const mesh::poly_mesh& working = *m_ctx->working();
		mesh::mesh_view& view = m_ctx->view();
		mesh::overlay_options options;
		options.xray = view.xray();
		options.picker = &view.picker(working);
		if (hovered) options.cursor = glm::vec2{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
		const mesh::overlay_frame frame = mesh::build_overlay(working, view.adjacency(working), view.selection(), view.hover(), screen, options);
		m_omitted_segments = frame.omitted_segments;

		const gui::theme_semantic_colors& palette = colors();
		const auto colour_of = [&palette](mesh::overlay_kind kind, float alpha) {
			ImVec4 base = palette.text_secondary;
			if (kind == mesh::overlay_kind::hover) base = palette.info;
			if (kind == mesh::overlay_kind::selected) base = palette.warning;
			return ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, base.w * alpha));
		};
		const auto at = [](const glm::vec2& p) { return ImVec2(p.x, p.y); };

		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->PushClipRect(image_pos, ImVec2(image_pos.x + image_size.x, image_pos.y + image_size.y), true);
		for (const mesh::overlay_polygon& polygon : frame.polygons) {
			const ImU32 fill = colour_of(polygon.kind, polygon.kind == mesh::overlay_kind::selected ? 0.35f : 0.2f);
			for (const auto& corners : polygon.triangles) {
				draw->AddTriangleFilled(at(polygon.corners[corners[0]]), at(polygon.corners[corners[1]]), at(polygon.corners[corners[2]]), fill);
			}
		}
		for (const mesh::overlay_segment& segment : frame.segments) {
			const bool plain = segment.kind == mesh::overlay_kind::plain;
			draw->AddLine(at(segment.from), at(segment.to), colour_of(segment.kind, plain ? 0.6f : 1.f), plain ? em(1.f) : em(2.f));
		}
		for (const mesh::overlay_point& point : frame.points) {
			const bool plain = point.kind == mesh::overlay_kind::plain;
			draw->AddCircleFilled(at(point.position), plain ? em(2.5f) : em(3.5f), colour_of(point.kind, 1.f));
		}
		if (m_gesture.dragging_box()) {
			const mesh::pixel_rect box = m_gesture.box();
			draw->AddRectFilled(at(box.min), at(box.max), colour_of(mesh::overlay_kind::hover, 0.15f));
			draw->AddRect(at(box.min), at(box.max), colour_of(mesh::overlay_kind::hover, 1.f));
		}
		draw->PopClipRect();
	}
}
