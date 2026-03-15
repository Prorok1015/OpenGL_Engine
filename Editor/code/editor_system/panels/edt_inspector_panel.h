#pragma once
#include "edt_panel_base.h"
#include "ecs_entity.h"
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace edt
{
	class inspector_panel : public panel_base
	{
	public:
		using component_draw_fn = std::function<void(entt::registry&, entt::entity)>;

		struct component_creator
		{
			std::string name;
			std::function<void(entt::registry&, entt::entity)> create_fn;
		};

		inspector_panel();

		void set_registry(std::shared_ptr<entt::registry> registry);
		void set_selected_entity(ecs::entity ent) { m_selected = ent; }

		// Register a renderer that checks for a component and draws its UI.
		// fn is called every frame for the selected entity — it should guard with all_of<T> internally.
		void add_component_renderer(component_draw_fn fn);

		// Register an entry in the "Add Component" popup.
		void add_component_creator(std::string name, std::function<void(entt::registry&, entt::entity)> fn);

	protected:
		void on_render() override;

	private:
		void draw_add_component_popup();

		std::shared_ptr<entt::registry> m_registry;
		ecs::entity m_selected = entt::null;
		std::vector<component_draw_fn> m_renderers;
		std::vector<component_creator> m_creators;
	};
}
