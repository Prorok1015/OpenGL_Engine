#include "scn_transform_system.h"
#include "common.h"
#include "scn_model.h"
#include "logger/engine_log.h"
#include <execution>

namespace {
    struct is_local_transform_invalidated {};
    struct is_transform_will_update_by_parent {};

    void calc_world_transforms(entt::entity ent, entt::registry& registry)
    {
        glm::mat4 local = glm::mat4{ 1.0 };
        glm::mat4 parent = glm::mat4{ 1.0 };

        if (registry.all_of<scn::parent_component>(ent))
        {
            auto& parent_ = registry.get<scn::parent_component>(ent);
            if (registry.all_of<scn::world_transform>(parent_.parent))
            {
                auto& parent_trans = registry.get<scn::world_transform>(parent_.parent);
                parent = parent_trans.world;
            }
        }

        if (registry.all_of<scn::local_transform>(ent))
        {
            auto& trans = registry.get<scn::local_transform>(ent);
            local = trans.local;
        }
        registry.emplace_or_replace<scn::world_transform>(ent, parent * local);

        registry.remove<is_local_transform_invalidated>(ent);
        registry.remove<is_transform_will_update_by_parent>(ent);

        if (registry.all_of<scn::children_component>(ent))
        {
            auto& children = registry.get<scn::children_component>(ent);
            for (auto& child : children.children)
            {
                calc_world_transforms(child, registry);
            }
        }
    }

    void update_transform_system(entt::registry& registry)
    {
        auto entts = registry.view<is_local_transform_invalidated>();
        std::for_each(std::execution::unseq, entts.begin(), entts.end(), [&](auto ent) { calc_world_transforms(ent, registry); });
    }

    void on_validate_local_transform(entt::registry& registry, entt::entity ent)
    {

        if (registry.all_of<is_transform_will_update_by_parent>(ent)) {
            return;
        }

        registry.remove<is_local_transform_invalidated>(ent);
        registry.emplace<is_transform_will_update_by_parent>(ent);
        if (registry.all_of<scn::children_component>(ent))
        {
            auto& children = registry.get<scn::children_component>(ent).children;
            for (auto& child : children)
            {
                on_validate_local_transform(registry, child);
            }
        }
    }

    void on_invalidate_local_transform(entt::registry& registry, ecs::entity ent)
    {
        if (registry.any_of<is_transform_will_update_by_parent, is_local_transform_invalidated>(ent)) {
            return;
        }

        registry.emplace<is_local_transform_invalidated>(ent);
        if (registry.all_of<scn::children_component>(ent))
        {
            auto& children = registry.get<scn::children_component>(ent).children;
            for (auto& child : children)
            {
                on_validate_local_transform(registry, child);
            }
        }
    }
}

void scn::init_transform_system(entt::registry& registry, entt::organizer& organizer)
{
    entt::sigh_helper{ registry }
        .with<scn::local_transform>()
        .on_construct<on_invalidate_local_transform>()
        .on_update<on_invalidate_local_transform>();

    organizer.emplace<update_transform_system>("update_transform_system");
}
