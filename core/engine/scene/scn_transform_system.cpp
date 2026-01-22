#include "scn_transform_system.h"
#include "common.h"
#include "scn_model.h"
#include "ecs_event.hpp"
#include "logger/engine_log.h"
#include <unordered_set>

namespace {

    void topological_sort(entt::entity ent,
                          std::vector<entt::entity>& sorted,
                          std::unordered_set<entt::entity>& visited,
                          entt::view<entt::get_t<const scn::children_component>>& children)
    {
        if (visited.contains(ent)) return;
        visited.insert(ent);

        sorted.push_back(ent);

        if (!children.contains(ent)) return;

        auto&& kids = children.get<0>(ent);
        for (auto&& child : kids.children) {
            topological_sort(child, sorted, visited, children);
        }
    }

    void update_depth_system(ecs::event<scn::hierarchy_updated>& event,
                             entt::view<entt::get_t<scn::depth_level>> depths,
                             entt::view<entt::get_t<const scn::parent_component>> parents,
                             entt::view<entt::get_t<const scn::children_component>> children)
    {
        std::unordered_set<entt::entity> visited;
        std::vector<entt::entity> sorted;

        for (auto&& ent : event) {
            topological_sort(ent, sorted, visited, children);
        }

        for (auto&& ent : sorted) {
            std::uint32_t depth = 0;
            if (parents.contains(ent)) {
                auto&& parent = parents.get<0>(ent);
                depth = depths.get<0>(parent.parent).value + 1;
            }
            depths.get<0>(ent).value = depth;
        }

        event.clear();
    }

    void update_world_matrix_recursive(entt::entity ent,
                                       entt::view<entt::get_t<scn::local_transform, scn::world_transform>>& view,
                                       entt::view<entt::get_t<const scn::parent_component>>& parents,
                                       entt::view<entt::get_t<const scn::children_component>>& children_list)
    {
        glm::mat4 parent_world{ 1.0 };
        if (parents.contains(ent)) {
            auto& parent = parents.get<0>(ent);
            parent_world = view.get<1>(parent.parent).world;
        }

        auto&& [local, world] = view.get(ent);
        world.world = parent_world * local.local;

        if (children_list.contains(ent)) {
            auto& kids = children_list.get<0>(ent);
            for (auto&& child : kids.children) {
                update_world_matrix_recursive(child, view, parents, children_list);
            }
        }
    }

    void update_transform_system(ecs::event<scn::transform_updated>& event,
                                 entt::view<entt::get_t<scn::local_transform, scn::world_transform>> view,
                                 entt::view<entt::get_t<const scn::depth_level>> depth,
                                 entt::view<entt::get_t<const scn::parent_component>> parents,
                                 entt::view<entt::get_t<const scn::children_component>> children)
    {
        std::sort(event.begin(), event.end(), [&] (auto&& a, auto&& b) {
            return depth.get<0>(a).value > depth.get<0>(b).value;
        });

        for (auto&& ent : event) {
            update_world_matrix_recursive(ent, view, parents, children);
        }

        event.clear();
    }
}

void scn::init_transform_system(ecs::system_factory& factory)
{
    factory.register_automatic_system<update_depth_system>("scn::depth_system");
    factory.register_automatic_system<update_transform_system>("scn::transform_system");
}
