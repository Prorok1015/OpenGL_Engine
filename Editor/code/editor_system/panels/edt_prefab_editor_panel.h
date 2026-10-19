#pragma once
#include "edt_panel_base.h"
#include "edt_input_binding.h"
#include <functional>
#include <memory>
#include <glm/glm.hpp>

namespace rnd { class render_system; }
namespace gui { class gui_system; }

namespace edt
{
	class prefab_editor_context;

	class prefab_editor_panel : public panel_base
	{
	public:
		prefab_editor_panel(rnd::render_system& rnd, gui::gui_system& gui);

		void set_context(prefab_editor_context* ctx) { m_ctx = ctx; }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		void set_input_router(inp::input_router* router) { m_router = router; }

	protected:
		void on_render() override;

	private:
		void render_toolbar();
		void render_viewport();
		void update_camera_viewport(float width, float height);

		rnd::render_system& m_rnd;
		gui::gui_system& m_gui;
		prefab_editor_context* m_ctx = nullptr;
		std::function<void()> m_on_close;
		inp::input_router* m_router = nullptr;
	};
}
