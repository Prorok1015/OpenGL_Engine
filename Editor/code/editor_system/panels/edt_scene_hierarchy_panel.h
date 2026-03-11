#pragma once
#include "edt_panel_base.h"
#include "ecs_entity.h"
#include <entt/entt.hpp>
#include <functional>
#include <memory>

namespace edt
{
	class scene_hierarchy_panel : public panel_base
	{
	public:
		scene_hierarchy_panel();

		void set_registry(std::shared_ptr<entt::registry> registry);
		void set_on_entity_selected(std::function<void(ecs::entity)> cb);
		ecs::entity get_selected_entity() const { return m_selected; }
		void set_selected_entity(ecs::entity ent) { m_selected = ent; }

	protected:
		void on_render() override;

	private:
		void draw_entity_node(ecs::entity ent);

		std::shared_ptr<entt::registry> m_registry;
		std::function<void(ecs::entity)> m_on_selected;
		ecs::entity m_selected = entt::null;
		char m_filter[128] = {};
	};
}
