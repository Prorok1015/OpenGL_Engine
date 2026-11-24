#include <snk/scene/scn_transform_system.h>
#include <snk/common/common.h>
#include <snk/scene/scn_model.h>
#include <snk/ecs/ecs_event.hpp>
#include <snk/common/logger/engine_log.h>
#include <unordered_set>

namespace {

    // Game code can destroy entities with a bare reg.destroy at any phase: they may still be queued in
    // transform_updated / hierarchy_updated, and their parent may still list them (#543). phase::transform is
    // the only consumer, so it skips them; a dead child is also dropped from its parent's list here.
    void drop_dead_children(entt::registry& registry, scn::children_component& kids)
    {
        std::erase_if(kids.children, [&](entt::entity child) { return !registry.valid(child); });
    }

    void topological_sort(entt::entity ent,
                          std::vector<entt::entity>& sorted,
                          std::unordered_set<entt::entity>& visited,
                          entt::registry& registry,
                          entt::view<entt::get_t<scn::children_component>>& children)
    {
        if (!registry.valid(ent)) return;
        if (visited.contains(ent)) return;
        visited.insert(ent);

        sorted.push_back(ent);

        if (!children.contains(ent)) return;

        auto&& kids = children.get<0>(ent);
        drop_dead_children(registry, kids);
        for (auto&& child : kids.children) {
            topological_sort(child, sorted, visited, registry, children);
        }
    }

    void update_depth_system(entt::registry& registry,
                             ecs::event<scn::hierarchy_updated>& event,
                             entt::view<entt::get_t<scn::depth_level>> depths,
                             entt::view<entt::get_t<const scn::parent_component>> parents,
                             entt::view<entt::get_t<scn::children_component>> children)
    {
        std::unordered_set<entt::entity> visited;
        std::vector<entt::entity> sorted;

        for (auto&& ent : event) {
            topological_sort(ent, sorted, visited, registry, children);
        }

        for (auto&& ent : sorted) {
            std::uint32_t depth = 0;
            if (parents.contains(ent)) {
                auto&& parent = parents.get<0>(ent);
                if (depths.contains(parent.parent)) {
                    depth = depths.get<0>(parent.parent).value + 1;
                }
            }
			if (depths.contains(ent)) {
                depths.get<0>(ent).value = depth;
            } else {
                depths.storage()->emplace<scn::depth_level>(ent, { depth });
            }
        }

        event.clear();
    }

    void update_world_matrix_recursive(entt::entity ent,
                                       entt::registry& registry,
                                       entt::view<entt::get_t<scn::local_transform, scn::world_transform>>& view,
                                       entt::view<entt::get_t<const scn::parent_component>>& parents,
                                       entt::view<entt::get_t<scn::children_component>>& children_list)
    {
        glm::mat4 parent_world{ 1.0 };
        if (parents.contains(ent)) {
            auto& parent = parents.get<0>(ent);
            if (view.contains(parent.parent)) {
                parent_world = view.get<1>(parent.parent).world;
            }
        }

        auto&& [local, world] = view.get(ent);
        world.world = parent_world * local.local;

        if (children_list.contains(ent)) {
            auto& kids = children_list.get<0>(ent);
            drop_dead_children(registry, kids);
            for (auto&& child : kids.children) {
                if (view.contains(child)) {
                    update_world_matrix_recursive(child, registry, view, parents, children_list);
                }
            }
        }
    }

    void update_transform_system(entt::registry& registry,
                                 ecs::event<scn::transform_updated>& event,
                                 entt::view<entt::get_t<scn::local_transform, scn::world_transform>> view,
                                 entt::view<entt::get_t<const scn::depth_level>> depth,
                                 entt::view<entt::get_t<const scn::parent_component>> parents,
                                 entt::view<entt::get_t<scn::children_component>> children)
    {
        // contains() is false for a destroyed entity and for one without local/world_transform.
        event.erase_if([&](entt::entity ent) { return !view.contains(ent); });

        const auto depth_of = [&](entt::entity ent) { return depth.contains(ent) ? depth.get<0>(ent).value : 0u; };
        std::sort(event.begin(), event.end(), [&](entt::entity a, entt::entity b) { return depth_of(a) > depth_of(b); });

        for (auto&& ent : event) {
            update_world_matrix_recursive(ent, registry, view, parents, children);
        }

        event.clear();
    }
}

void scn::init_transform_system(ecs::system_factory& factory)
{
    factory.register_system<update_depth_system>("scn::depth_system",
        { .phase = ecs::phase::transform, .run_in_edit = true });
    factory.register_system<update_transform_system>("scn::transform_system",
        { .phase = ecs::phase::transform, .after = { "scn::depth_system" }, .run_in_edit = true });
}
