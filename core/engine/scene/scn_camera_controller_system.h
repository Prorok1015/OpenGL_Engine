#pragma once
#include "ecs_system.h"
#include "ecs_entity.h"

namespace scn
{
	class mouse_controller_job : public ecs::job_base
	{
	public:
		mouse_controller_job() = default;

		// Inherited via job_base
		void init(entt::organizer& organizer, entt::registry& registry) override;

		void deinit(entt::organizer& organizer, entt::registry& registry) override;

	private:
		void on_update_mouse_controller_by_mouse_click_event(entt::registry& registry, ecs::entity ent);
		void on_update_mouse_controller_by_mouse_move_event(entt::registry& registry, ecs::entity ent);
		void on_update_mouse_controller_by_scroll_event(entt::registry& registry, ecs::entity ent);
		void on_update_mouse_controller_local_transform(entt::registry& registry, ecs::entity ent);
	};
}