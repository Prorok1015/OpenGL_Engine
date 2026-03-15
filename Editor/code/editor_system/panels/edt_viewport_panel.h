#pragma once
#include "edt_panel_base.h"
#include "ecs_entity.h"

#include "../edt_input_manager.h"
#include "../edt_transform_gizmo.hpp"
#include "inp_ecs_input_manager.h"
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <string>

namespace edt
{
	class viewport_panel : public panel_base
	{
	public:
		viewport_panel();


		void set_registry(std::shared_ptr<entt::registry> registry);
		void set_selected_entity(ecs::entity ent) { m_selected = ent; }

		void set_input_manager(std::shared_ptr<edt::input_manager> input) { m_input = std::move(input); }
		void set_ecs_input_manager(std::shared_ptr<inp::ecs_input_manager> ecs_input) { m_ecs_input = std::move(ecs_input); }

		// Called when a file is dropped onto the viewport from the Asset Browser.
		// Argument: absolute filesystem path of the dropped asset.
		void set_on_asset_dropped(std::function<void(const std::string&)> cb) { m_on_asset_dropped = std::move(cb); }

		glm::vec2 get_viewport_pos() const { return m_image_pos; }
		glm::vec2 get_viewport_size() const { return m_viewport_size; }
		bool is_focused() const { return m_focused; }

	protected:
		void on_render() override;

	private:
		void render_toolbar();
		void render_fps_overlay();
		void render_orientation_gizmo();
		void render_transform_gizmo(const glm::mat4& view, const glm::mat4& proj);


		std::shared_ptr<entt::registry> m_registry;
		std::shared_ptr<edt::input_manager> m_input;
		std::shared_ptr<inp::ecs_input_manager> m_ecs_input;
		ecs::entity m_selected = entt::null;
		glm::vec2 m_image_pos = {};      // screen pos of the scene image (after toolbar)
		glm::vec2 m_viewport_size = {800.f, 600.f};
		bool m_focused = false;
		int m_gizmo_op = 0; // 0=translate, 1=rotate, 2=scale
		float m_fps = 0.f;
		float m_fps_timer = 0.f;
		int m_frame_count = 0;
		std::function<void(const std::string&)> m_on_asset_dropped;
	};
}
