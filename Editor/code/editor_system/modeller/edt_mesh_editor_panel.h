#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_input_binding.h"
#include "edt_mesh_gesture.h"
#include "edt_mesh_gizmo.h"
#include "edt_modeller_settings.h"
#include "edt_mesh_picking.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace rnd { class render_system; }
namespace gui { class gui_system; }
struct ImVec2;

namespace edt
{
	class mesh_document;

	// The window of the mesh modeller: the toolbar, the banners about the files and the preview of the shown mesh. It shows one
	// document at a time (the active one); the component says which.
	class mesh_editor_panel : public panel_base
	{
	public:
		mesh_editor_panel(rnd::render_system& rnd, gui::gui_system& gui);

		// The shown document; the one shown before stops drawing. Null: none.
		void set_context(mesh_document* shown);
		mesh_document* context() const { return m_ctx; }
		void set_on_save(std::function<void()> cb) { m_on_save = std::move(cb); }
		void set_on_revert(std::function<void()> cb) { m_on_revert = std::move(cb); }
		void set_on_overwrite_geometry(std::function<void()> cb) { m_on_overwrite_geometry = std::move(cb); }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		// What the Close button does (tests press it here).
		void press_close() { if (m_on_close) m_on_close(); }
		void set_input_router(inp::input_router* router) { m_router = router; }
		// The snap steps (Editor Settings); read when a drag starts and while it runs. Null: the defaults.
		void set_settings_source(std::function<modeller_settings()> source) { m_settings = std::move(source); }
		mesh::gizmo_tool tool() const { return m_tool; }
		mesh::gizmo_orientation orientation() const { return m_orientation; }
		bool dragging() const { return m_drag.has_value(); }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		void render_toolbar();
		void render_banners();
		void render_viewport();
		void render_mode_bar();
		void render_transform_bar();
		void apply_numbers(const mesh::pivot_frame& frame);
		mesh::gizmo_metrics gizmo_metrics_now() const;
		std::optional<mesh::pivot_frame> current_frame() const;
		// True when the gizmo has the mouse (hovered, or a drag runs): the selection gesture must not begin.
		bool update_gizmo(const mesh::screen_view& screen, bool hovered, const glm::vec2& mouse);
		void begin_drag(const mesh::screen_view& screen, const mesh::pivot_frame& frame, const glm::vec2& mouse);
		void end_drag(bool commit);
		void draw_gizmo(const ImVec2& image_pos, const ImVec2& image_size);
		void handle_mode_keys();
		void interact_with_image(const mesh::screen_view& screen, bool hovered);
		void draw_overlay(const mesh::screen_view& screen, const ImVec2& image_pos, const ImVec2& image_size, bool hovered);
		void release_camera();
		// The camera reads the mouse again (the gesture stays as it is).
		void restore_camera_input();

		rnd::render_system& m_rnd;
		gui::gui_system& m_gui;
		mesh_document* m_ctx = nullptr;
		inp::input_router* m_router = nullptr;
		std::function<void()> m_on_save;
		std::function<void()> m_on_revert;
		std::function<void()> m_on_overwrite_geometry;
		std::function<void()> m_on_close;
		std::string m_error;
		mesh::select_gesture m_gesture;
		std::size_t m_omitted_segments = 0;
		std::function<modeller_settings()> m_settings;
		mesh::gizmo_tool m_tool = mesh::gizmo_tool::move;
		mesh::gizmo_orientation m_orientation = mesh::gizmo_orientation::world;
		std::optional<mesh::gizmo_layout> m_layout;
		mesh::gizmo_part m_hot_part = mesh::gizmo_part::none;
		std::optional<mesh::gizmo_drag> m_drag;
		mesh::pivot_frame m_drag_frame;
		mesh::vertex_snapshot m_snapshot;
		mesh::gizmo_delta m_drag_delta;
		// What the numbers panel shows, and what it was made from.
		glm::vec3 m_numbers{ 0.f };
		// What the numbers were made from: they are made again when any of it changes and no field is being edited.
		struct numbers_key
		{
			mesh::gizmo_tool tool = mesh::gizmo_tool::move;
			mesh::gizmo_orientation orientation = mesh::gizmo_orientation::world;
			const mesh_document* document = nullptr;
			std::uint64_t generation = 0;
			std::uint64_t selection = 0;
			glm::vec3 origin{ 0.f };
			bool valid = false;
			bool operator==(const numbers_key&) const = default;
		};
		numbers_key m_numbers_key;
		bool m_numbers_editing = false;
		// What the hover was computed from: it is looked for again only when one of these changed.
		struct hover_key
		{
			glm::vec2 mouse{ 0.f };
			glm::mat4 view_projection{ 1.f };
			glm::vec2 image_origin{ 0.f };
			glm::vec2 image_size{ 0.f };
			std::uint64_t generation = 0;
			mesh::select_mode mode = mesh::select_mode::vertex;
			bool pointing = false;

			bool operator==(const hover_key&) const = default;
		};
		std::optional<hover_key> m_hover_key;
	};
}
