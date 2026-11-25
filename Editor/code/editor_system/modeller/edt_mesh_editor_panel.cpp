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
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <vector>
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
		if (m_drag) end_drag(false);
		m_layout.reset();
		m_hot_part = mesh::gizmo_part::none;
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
		render_transform_bar();
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
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !m_drag) {
				if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_tool = mesh::gizmo_tool::move;
				if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_tool = mesh::gizmo_tool::rotate;
				if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_tool = mesh::gizmo_tool::scale;
			}
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
				draw_gizmo(image_pos, size);
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

		m_gesture.set_drag_threshold(em(4.f));
		const bool gizmo_has_mouse = update_gizmo(screen, hovered, mouse);
		if (gizmo_has_mouse) {
			if (scn::mouse_controller_component* controller = m_ctx->camera_controller()) controller->input_enabled = !m_drag.has_value();
			if (!m_drag) view.set_hover({});
			m_hover_key.reset();
			return;
		}
		const mesh::pick_query query = view.query(working, screen);
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

	mesh::gizmo_metrics mesh_editor_panel::gizmo_metrics_now() const
	{
		mesh::gizmo_metrics metrics;
		metrics.arm = em(90.f);
		metrics.pick = em(7.f);
		metrics.cube = em(5.f);
		metrics.centre = em(8.f);
		return metrics;
	}

	std::optional<mesh::pivot_frame> mesh_editor_panel::current_frame() const
	{
		if (!m_ctx || !m_ctx->working()) return std::nullopt;
		const mesh::poly_mesh& working = *m_ctx->working();
		if (m_drag && m_drag->tool() != mesh::gizmo_tool::move) return m_drag_frame;
		return mesh::selection_frame(working, m_ctx->view().current_selection(working), mesh::pivot_mode::median, m_orientation);
	}

	void mesh_editor_panel::begin_drag(const mesh::screen_view& screen, const mesh::pivot_frame& frame, const glm::vec2& mouse)
	{
		if (!m_layout || m_ctx->edits_locked()) return;
		std::optional<mesh::gizmo_drag> drag = mesh::gizmo_drag::begin(screen, *m_layout, m_hot_part, mouse);
		if (!drag) return;
		const char* label = m_tool == mesh::gizmo_tool::move ? "Move" : m_tool == mesh::gizmo_tool::rotate ? "Rotate" : "Scale";
		if (!m_ctx->begin_gesture(label)) return;
		const mesh::poly_mesh& working = *m_ctx->working();
		m_snapshot = mesh::capture_vertices(working, mesh::selected_vertices(working, m_ctx->view().current_selection(working)));
		m_drag_frame = frame;
		m_drag_delta = {};
		m_drag = std::move(drag);
	}

	void mesh_editor_panel::end_drag(bool commit)
	{
		m_drag.reset();
		m_snapshot = {};
		if (!m_ctx || !m_ctx->gesture_active()) return;
		if (commit) {
			m_ctx->end_gesture();
		} else {
			m_ctx->cancel_gesture();
		}
	}

	bool mesh_editor_panel::update_gizmo(const mesh::screen_view& screen, bool hovered, const glm::vec2& mouse)
	{
		m_layout.reset();
		m_hot_part = mesh::gizmo_part::none;
		const ImGuiIO& io = ImGui::GetIO();
		if (m_drag && !m_ctx->gesture_active()) m_drag.reset();
		const std::optional<mesh::pivot_frame> frame = current_frame();
		if (!frame) {
			if (m_drag) end_drag(false);
			return false;
		}
		m_layout = mesh::make_gizmo_layout(screen, *frame, m_tool, gizmo_metrics_now());
		if (!m_layout) {
			if (m_drag) end_drag(false);
			return false;
		}

		if (m_drag) {
			m_hot_part = m_drag->part();
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				end_drag(false);
				return true;
			}
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				end_drag(true);
				return true;
			}
			const modeller_settings steps = m_settings ? clamped(m_settings()) : modeller_settings{};
			mesh::gizmo_snap snap;
			if (io.KeyCtrl) {
				snap.move = steps.move_step;
				snap.rotate_radians = steps.rotate_step_degrees * std::numbers::pi_v<float> / 180.f;
				snap.scale = steps.scale_step;
			}
			if (const auto delta = m_drag->update(screen, mouse, snap); delta && delta->matrix != m_drag_delta.matrix) {
				m_drag_delta = *delta;
				m_ctx->gesture_edit([this, &delta](mesh::poly_mesh& working) { return mesh::apply_transform(working, m_snapshot, delta->matrix) > 0; });
			}
			return true;
		}

		if (!hovered || m_gesture.active() || io.KeyAlt || ImGui::IsMouseDown(ImGuiMouseButton_Right)) return false;
		m_hot_part = mesh::pick_gizmo_part(*m_layout, mouse, gizmo_metrics_now());
		if (m_hot_part == mesh::gizmo_part::none) return false;
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) begin_drag(screen, *frame, mouse);
		return true;
	}

	void mesh_editor_panel::draw_gizmo(const ImVec2& image_pos, const ImVec2& image_size)
	{
		if (!m_layout) return;
		const gui::theme_semantic_colors& palette = colors();
		const auto colour_of = [&](mesh::gizmo_part part, float visibility) {
			ImVec4 base = palette.text_secondary;
			switch (part) {
			case mesh::gizmo_part::x:
			case mesh::gizmo_part::yz:
				base = palette.error;
				break;
			case mesh::gizmo_part::y:
			case mesh::gizmo_part::zx:
				base = palette.success;
				break;
			case mesh::gizmo_part::z:
			case mesh::gizmo_part::xy:
				base = palette.info;
				break;
			default:
				break;
			}
			if (part == m_hot_part) base = palette.warning;
			return ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, base.w * visibility));
		};
		const auto at = [](const glm::vec2& p) { return ImVec2(p.x, p.y); };
		const mesh::gizmo_layout& layout = *m_layout;
		const mesh::gizmo_metrics metrics = gizmo_metrics_now();

		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->PushClipRect(image_pos, ImVec2(image_pos.x + image_size.x, image_pos.y + image_size.y), true);
		for (const mesh::gizmo_shape& shape : layout.shapes) {
			if (shape.visibility < 0.02f) continue;
			const ImU32 colour = colour_of(shape.part, shape.visibility);
			const ImU32 fill = colour_of(shape.part, shape.visibility * 0.35f);
			switch (shape.part) {
			case mesh::gizmo_part::x:
			case mesh::gizmo_part::y:
			case mesh::gizmo_part::z:
				if (layout.tool == mesh::gizmo_tool::rotate) {
					if (shape.points.size() > 2) {
						std::vector<ImVec2> ring;
						ring.reserve(shape.points.size());
						for (const glm::vec2& point : shape.points) ring.push_back(at(point));
						draw->AddPolyline(ring.data(), static_cast<int>(ring.size()), colour, ImDrawFlags_Closed, em(2.f));
					}
				} else if (shape.points.size() == 2) {
					draw->AddLine(at(shape.points[0]), at(shape.points[1]), colour, em(2.f));
					if (layout.tool == mesh::gizmo_tool::move) {
						const glm::vec2 direction = glm::normalize(shape.points[1] - shape.points[0]);
						const glm::vec2 side{ -direction.y, direction.x };
						const glm::vec2 tip = shape.points[1] + direction * em(10.f);
						draw->AddTriangleFilled(at(tip), at(shape.points[1] + side * em(4.f)), at(shape.points[1] - side * em(4.f)), colour);
					} else {
						const float half = metrics.cube;
						draw->AddRectFilled(ImVec2(shape.anchor.x - half, shape.anchor.y - half), ImVec2(shape.anchor.x + half, shape.anchor.y + half), colour);
					}
				}
				break;
			case mesh::gizmo_part::yz:
			case mesh::gizmo_part::zx:
			case mesh::gizmo_part::xy:
				if (shape.points.size() == 4) {
					draw->AddQuadFilled(at(shape.points[0]), at(shape.points[1]), at(shape.points[2]), at(shape.points[3]), fill);
					draw->AddQuad(at(shape.points[0]), at(shape.points[1]), at(shape.points[2]), at(shape.points[3]), colour, em(1.f));
				}
				break;
			case mesh::gizmo_part::centre:
				draw->AddCircleFilled(at(shape.anchor), em(4.f), colour);
				draw->AddCircle(at(shape.anchor), metrics.centre, colour, 0, em(1.f));
				break;
			case mesh::gizmo_part::screen:
				draw->AddCircle(at(shape.anchor), layout.screen_ring_radius, colour, 64, em(1.5f));
				break;
			default:
				break;
			}
		}
		if (m_drag) {
			const mesh::gizmo_delta& delta = m_drag_delta;
			std::string text;
			switch (m_drag->tool()) {
			case mesh::gizmo_tool::move:
				text = std::format("{:+.3f}  {:+.3f}  {:+.3f}", delta.translation.x, delta.translation.y, delta.translation.z);
				break;
			case mesh::gizmo_tool::rotate:
				text = std::format("{:+.1f} deg", delta.angle_radians * 180.f / std::numbers::pi_v<float>);
				break;
			case mesh::gizmo_tool::scale:
				text = std::format("x{:.3f}  x{:.3f}  x{:.3f}", delta.factors.x, delta.factors.y, delta.factors.z);
				break;
			}
			draw->AddText(ImVec2(layout.origin.x + em(14.f), layout.origin.y + em(14.f)), ImGui::ColorConvertFloat4ToU32(palette.warning), text.c_str());
		}
		draw->PopClipRect();
	}

	void mesh_editor_panel::render_transform_bar()
	{
		const mesh::poly_mesh* working = m_ctx->working();
		if (!working) return;
		const auto tool_button = [&](const char* label, mesh::gizmo_tool value, const char* tip) {
			if (ImGui::RadioButton(label, m_tool == value) && !m_drag) m_tool = value;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
			ImGui::SameLine();
		};
		tool_button("Move", mesh::gizmo_tool::move, "Move the selection (W)");
		tool_button("Rotate", mesh::gizmo_tool::rotate, "Rotate the selection (E)");
		tool_button("Scale", mesh::gizmo_tool::scale, "Scale the selection (R)");
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		const auto orientation_button = [&](const char* label, mesh::gizmo_orientation value, const char* tip) {
			if (ImGui::RadioButton(label, m_orientation == value) && !m_drag) m_orientation = value;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
			ImGui::SameLine();
		};
		orientation_button("World", mesh::gizmo_orientation::world, "The gizmo follows the world's axes");
		orientation_button("Normal", mesh::gizmo_orientation::normal, "The gizmo's Z is the average normal of the selection");
		ImGui::TextDisabled("|");

		const std::optional<mesh::pivot_frame> frame = current_frame();
		const numbers_key key{ m_tool, m_orientation, m_ctx, working->generation(), mesh::selection_stamp(m_ctx->view().current_selection(*working)),
			frame ? frame->origin : glm::vec3(0.f), frame.has_value() };
		if (!m_numbers_editing && !(m_numbers_key == key)) {
			if (frame) m_numbers = mesh::numeric_defaults(m_tool, *frame);
			m_numbers_key = key;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!frame || m_ctx->edits_locked() || m_drag.has_value());
		const char* axis_labels[3] = { "X", "Y", "Z" };
		bool apply = false;
		bool any_active = false;
		for (int i = 0; i < 3; ++i) {
			ImGui::SetNextItemWidth(em(80.f));
			char id[8];
			std::snprintf(id, sizeof(id), "%s##num", axis_labels[i]);
			apply |= ImGui::InputFloat(id, &m_numbers[i], 0.f, 0.f, m_tool == mesh::gizmo_tool::rotate ? "%.2f" : "%.4f", ImGuiInputTextFlags_EnterReturnsTrue);
			any_active |= ImGui::IsItemActive();
			ImGui::SameLine();
		}
		ImGui::EndDisabled();
		m_numbers_editing = any_active;
		ImGui::TextDisabled("%s", m_tool == mesh::gizmo_tool::move ? "position" : m_tool == mesh::gizmo_tool::rotate ? "degrees" : "factors");
		if (apply && frame) {
			// Enter on a field nobody changed is no edit: it would only add a record that moves nothing.
			if (m_numbers != mesh::numeric_defaults(m_tool, *frame)) {
				m_error = mesh::numeric_problem(m_tool, m_numbers);
				if (m_error.empty()) apply_numbers(*frame);
			}
			m_numbers_key = {};
		}
	}

	void mesh_editor_panel::apply_numbers(const mesh::pivot_frame& frame)
	{
		const mesh::poly_mesh* working = m_ctx->working();
		if (!working) return;
		const mesh::vertex_snapshot snapshot = mesh::capture_vertices(*working, mesh::selected_vertices(*working, m_ctx->view().current_selection(*working)));
		const glm::mat4 matrix = mesh::numeric_matrix(m_tool, frame, m_numbers);
		const char* label = m_tool == mesh::gizmo_tool::move ? "Move" : m_tool == mesh::gizmo_tool::rotate ? "Rotate" : "Scale";
		m_ctx->edit([&](mesh::poly_mesh& target) { return mesh::apply_transform(target, snapshot, matrix) > 0; }, label);
	}
}
