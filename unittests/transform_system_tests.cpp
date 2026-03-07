
#include <boost/test/unit_test.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>
#include <entt/entt.hpp>
#include "scn_model.h"
#include "ecs_event.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_set>

// скопированные и адаптированные функции из scn_transform_system.cpp
namespace {

    void topological_sort(entt::entity ent,
                          std::vector<entt::entity>& sorted,
                          std::unordered_set<entt::entity>& visited,
                          entt::registry& registry)
    {
        auto children_view = registry.view<const scn::children_component>();
        if (visited.count(ent)) return;
        visited.insert(ent);

        sorted.push_back(ent);

        if (!children_view.contains(ent)) return;

        auto&& kids = children_view.get<const scn::children_component>(ent);
        for (auto&& child : kids.children) {
            topological_sort(child, sorted, visited, registry);
        }
    }

    void update_depth_system(ecs::event<scn::hierarchy_updated>& event,
                             entt::registry& registry)
    {
        auto depths = registry.view<scn::depth_level>();
        auto parents = registry.view<const scn::parent_component>();

        std::unordered_set<entt::entity> visited;
        std::vector<entt::entity> sorted;

        for (auto&& ent : event) {
            topological_sort(ent, sorted, visited, registry);
        }

        for (auto&& ent : sorted) {
            std::uint32_t depth = 0;
            if (parents.contains(ent)) {
                auto&& parent = parents.get<const scn::parent_component>(ent);
                if (depths.contains(parent.parent)) {
                    depth = depths.get<scn::depth_level>(parent.parent).value + 1;
                }
            }
            if(depths.contains(ent))
            {
                depths.get<scn::depth_level>(ent).value = depth;
            }
        }

        event.clear();
    }

    void update_world_matrix_recursive(entt::entity ent,
                                       entt::registry& registry)
    {
        auto view = registry.view<scn::local_transform, scn::world_transform>();
        auto parents = registry.view<const scn::parent_component>();
        auto children_list = registry.view<const scn::children_component>();

        glm::mat4 parent_world{ 1.0 };
        if (parents.contains(ent)) {
            auto& parent = parents.get<const scn::parent_component>(ent);
            if (view.contains(parent.parent)) {
                parent_world = view.get<scn::world_transform>(parent.parent).world;
            }
        }

        if (view.contains(ent)) {
            auto&& [local, world] = view.get(ent);
            world.world = parent_world * local.local;
        }

        if (children_list.contains(ent)) {
            auto& kids = children_list.get<const scn::children_component>(ent);
            for (auto&& child : kids.children) {
                update_world_matrix_recursive(child, registry);
            }
        }
    }

    void update_transform_system(ecs::event<scn::transform_updated>& event,
                                 entt::registry& registry)
    {
        auto depth = registry.view<const scn::depth_level>();
        
        std::sort(event.begin(), event.end(), [&] (auto&& a, auto&& b) {
            const auto& depth_a = depth.get<const scn::depth_level>(a).value;
            const auto& depth_b = depth.get<const scn::depth_level>(b).value;
            return depth_a > depth_b;
        });

        for (auto&& ent : event) {
            update_world_matrix_recursive(ent, registry);
        }

        event.clear();
    }
}

BOOST_AUTO_TEST_SUITE(TransformSystemTests)

BOOST_AUTO_TEST_CASE(DepthSystem_CorrectlyCalculatesDepth)
{
    entt::registry registry;
    
    auto root = registry.create();
    auto child = registry.create();
    auto grandchild = registry.create();

    registry.emplace<scn::children_component>(root, scn::children_component{{child}});
    registry.emplace<scn::parent_component>(child, scn::parent_component{root});
    registry.emplace<scn::children_component>(child, scn::children_component{{grandchild}});
    registry.emplace<scn::parent_component>(grandchild, scn::parent_component{child});

    registry.emplace<scn::depth_level>(root);
    registry.emplace<scn::depth_level>(child);
    registry.emplace<scn::depth_level>(grandchild);
    
    ecs::event<scn::hierarchy_updated> hierarchy_updated_event;
    hierarchy_updated_event.emit(root);

    update_depth_system(hierarchy_updated_event, registry);

    BOOST_TEST(registry.get<scn::depth_level>(root).value == 0);
    BOOST_TEST(registry.get<scn::depth_level>(child).value == 1);
    BOOST_TEST(registry.get<scn::depth_level>(grandchild).value == 2);
}

BOOST_AUTO_TEST_CASE(TransformSystem_CorrectlyUpdatesWorldTransform)
{
    entt::registry registry;

    auto parent = registry.create();
    auto child = registry.create();

    registry.emplace<scn::children_component>(parent, scn::children_component{{child}});
    registry.emplace<scn::parent_component>(child, scn::parent_component{parent});
    
    registry.emplace<scn::depth_level>(parent, 0u);
    registry.emplace<scn::depth_level>(child, 1u);

    registry.emplace<scn::local_transform>(parent);
    registry.emplace<scn::world_transform>(parent);
    registry.emplace<scn::local_transform>(child);
    registry.emplace<scn::world_transform>(child);

    auto& parent_local_transform = registry.get<scn::local_transform>(parent);
    parent_local_transform.local = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));

    // Обновляем родительскую матрицу
    update_world_matrix_recursive(parent, registry);

    auto& child_local_transform = registry.get<scn::local_transform>(child);
    child_local_transform.local = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    
    ecs::event<scn::transform_updated> transform_updated_event;
    transform_updated_event.emit(child);
    
    update_transform_system(transform_updated_event, registry);

    const auto& parent_world_transform = registry.get<scn::world_transform>(parent).world;
    const auto& child_world_transform = registry.get<scn::world_transform>(child).world;
    glm::mat4 expected_transform = parent_world_transform * child_local_transform.local;

    const float epsilon = 1e-6f;
    for(int i = 0; i < 4; ++i) {
        for(int j = 0; j < 4; ++j) {
            BOOST_CHECK_SMALL(expected_transform[i][j] - child_world_transform[i][j], epsilon);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
