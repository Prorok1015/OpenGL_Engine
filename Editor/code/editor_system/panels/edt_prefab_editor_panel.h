#pragma once
#include "edt_panel_base.h"
#include <functional>
#include <memory>
#include <glm/glm.hpp>

namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace inp { class ecs_input_manager; }

namespace edt
{
	class prefab_editor_context;
	class input_manager;

	class prefab_editor_panel : public panel_base
	{
	public:
		prefab_editor_panel(rnd::render_system& rnd, gui::gui_system& gui);

		void set_context(prefab_editor_context* ctx) { m_ctx = ctx; }
		void set_on_close(std::function<void()> cb) { m_on_close = std::move(cb); }
		void set_ecs_input_manager(std::shared_ptr<inp::ecs_input_manager> mgr) { m_ecs_input = std::move(mgr); }
		void set_editor_input(std::shared_ptr<edt::input_manager> mgr) { m_editor_input = std::move(mgr); }

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
		std::shared_ptr<inp::ecs_input_manager> m_ecs_input;
		std::shared_ptr<edt::input_manager> m_editor_input;
	};
}
