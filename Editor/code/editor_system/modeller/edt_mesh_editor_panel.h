#pragma once
#include <snk/editor/edt_panel_base.h>
#include "edt_input_binding.h"
#include "edt_mesh_gesture.h"
#include "edt_mesh_picking.h"
#include <cstddef>
#include <functional>
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

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		void render_toolbar();
		void render_banners();
		void render_viewport();
		void render_mode_bar();
		void handle_mode_keys();
		void interact_with_image(const mesh::screen_view& screen, bool hovered);
		void draw_overlay(const mesh::screen_view& screen, const ImVec2& image_pos, const ImVec2& image_size, bool hovered);
		void release_camera();

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
	};
}
