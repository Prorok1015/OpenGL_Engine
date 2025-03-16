#pragma once
#include "common.h"
#include "ecs_system.h"
#include "desc_system.h"

namespace desc
{
	class desc_load_job : public ecs::job_base
	{
	public:
		desc_load_job() = default;

		void init(entt::organizer& organizer, entt::registry& registry) override;
		void deinit(entt::organizer& organizer, entt::registry& registry) override;

		void internal_init(desc::desc_system* desc_system) {
			this->desc_system = desc_system;
		}

	private:
		void update();

	private:
		desc::desc_system* desc_system;
	};
}