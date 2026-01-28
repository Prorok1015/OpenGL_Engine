#include "scn_level.h"
#include "scn_model.h"
#include "ecs_command_buffer.hpp"
#include "ecs_entity_changer.hpp"
#include "ecs_component.h"
#include <entt/fwd.hpp>

void scn::level::update(std::chrono::duration<float> dt)
{
    m_accumulator += dt.count();

    while (m_accumulator >= m_fixed_step) {
        m_level_state.ctx().insert_or_assign<scn::fixed_time>({ m_fixed_step });

        run_graph(m_fixed_graph);

        m_accumulator -= m_fixed_step;
    }

    m_level_state.ctx().insert_or_assign<delta_time>({ dt.count() });

    float alpha = m_accumulator / m_fixed_step;
    m_level_state.ctx().insert_or_assign<scn::update_alpha>({ alpha });

    run_graph(m_variable_graph);
    sync_point();
}

void scn::level::sync_point()
{
    for (auto& [name, world] : m_worlds) {
        auto& state = world->state();
        if (auto* changer = state.ctx().find<ecs::entity_changer>()) {
            changer->apply(state);
        }

        if (auto* sandbox = state.ctx().find<ecs::entity_spawner>()) {
            entt::continuous_loader loader{ state };
            {
                const auto& entity_storage = sandbox->storage<ecs::sandbox_entity>();
                ecs::sandbox_loader_archive archive{ loader, entity_storage };
                loader.get<entt::entity>(archive);
            }

            for (auto [id, storage] : sandbox->storage()) {
                if (id == entt::type_hash<ecs::sandbox_entity>::value()) continue;
                if (storage.empty()) continue;

                if (auto type = entt::resolve(id)) {
                    if (auto func = type.func(ecs::loader_get_h)) {
                        func.invoke({}, &loader, static_cast<const entt::basic_sparse_set<ecs::sandbox_entity>*>(&storage));
                    }
                }
            }

            sandbox->clear();
        }
    }
}
