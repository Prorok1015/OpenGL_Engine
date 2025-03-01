#pragma once
#include "ecs_system.h"
#include "ecs_entity.h"

namespace scn
{
    class transform_job : public ecs::job_base
    {
    public:
        transform_job() = default;

        void init(entt::organizer& organizer, entt::registry& registry) override;

        void deinit(entt::organizer& organizer, entt::registry& registry) override;

        void update_transform_system(entt::registry& registry);

        void calc_world_transforms(ecs::entity ent, entt::registry& registry);

        void on_validate_local_transform(entt::registry& registry, ecs::entity ent);

        void on_invalidate_local_transform(entt::registry& registry, ecs::entity ent);
    };

	void update_animation_system(float time_second);
	void update_bone_offsets_system(float time_second);
}