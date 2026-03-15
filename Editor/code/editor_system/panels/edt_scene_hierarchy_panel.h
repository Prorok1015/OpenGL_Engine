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
		// Returns display name for an entity ("ComponentIcon Name" or "Entity #N")
		std::string get_display_name(ecs::entity ent) const;
		// Returns a short type-prefix like "[M]", "[C]", "[L]", "[A]" etc.
		const char* get_type_prefix(ecs::entity ent) const;
		// Draw one node and recurse into its children.
		void draw_entity_node(ecs::entity ent);
		// Recursively destroy an entity and all its descendants, cleaning parent links.
		void destroy_entity(ecs::entity ent);

		std::shared_ptr<entt::registry> m_registry;
		std::function<void(ecs::entity)> m_on_selected;
		ecs::entity m_selected       = entt::null;
		ecs::entity m_rename_entity  = entt::null;
		char m_filter[128]           = {};
		char m_rename_buf[256]       = {};
	};
}
