#pragma once
#include "edt_panel_base.h"
#include "ecs_entity.h"
#include <entt/entt.hpp>
#include <memory>

namespace edt
{
	class inspector_panel : public panel_base
	{
	public:
		inspector_panel();

		void set_registry(std::shared_ptr<entt::registry> registry);
		void set_selected_entity(ecs::entity ent) { m_selected = ent; }

	protected:
		void on_render() override;

	private:
		void draw_transform_component();
		void draw_camera_component();
		void draw_add_component_popup();

		std::shared_ptr<entt::registry> m_registry;
		ecs::entity m_selected = entt::null;
	};
}
