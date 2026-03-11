#pragma once
#include "edt_panel_base.h"
#include "ecs_entity.h"
#include "scn_renderer.h"
#include "../edt_input_manager.h"
#include "inp_ecs_input_manager.h"
#include <glm/glm.hpp>
#include <memory>

namespace edt
{
	class viewport_panel : public panel_base
	{
	public:
		viewport_panel();

		void set_renderer(std::shared_ptr<scn::renderer_3d> renderer);
		void set_registry(std::shared_ptr<entt::registry> registry);
		void set_selected_entity(ecs::entity ent) { m_selected = ent; }

		void set_input_manager(std::shared_ptr<edt::input_manager> input) { m_input = std::move(input); }
		void set_ecs_input_manager(std::shared_ptr<inp::ecs_input_manager> ecs_input) { m_ecs_input = std::move(ecs_input); }

		glm::vec2 get_viewport_pos() const { return m_viewport_pos; }
		glm::vec2 get_viewport_size() const { return m_viewport_size; }
		bool is_focused() const { return m_focused; }

	protected:
		void on_render() override;

	private:
		void render_toolbar();
		void render_fps_overlay();

		std::shared_ptr<scn::renderer_3d> m_renderer;
		std::shared_ptr<entt::registry> m_registry;
		std::shared_ptr<edt::input_manager> m_input;
		std::shared_ptr<inp::ecs_input_manager> m_ecs_input;
		ecs::entity m_selected = entt::null;
		glm::vec2 m_viewport_pos = {};
		glm::vec2 m_viewport_size = {800.f, 600.f};
		bool m_focused = false;
		int m_gizmo_op = 0; // 0=translate, 1=rotate, 2=scale
		float m_fps = 0.f;
		float m_fps_timer = 0.f;
		int m_frame_count = 0;
	};
}
