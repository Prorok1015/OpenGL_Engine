#include "scn_world.h"
#include "scn_model.h"

void scn::world::update(std::chrono::duration<float> dt)
{
    m_accumulator += dt.count();

    while (m_accumulator >= m_fixed_step) {
        m_state.ctx().insert_or_assign<scn::fixed_time>({ m_fixed_step });

        run_graph(m_fixed_graph);

        m_accumulator -= m_fixed_step;
    }

    m_state.ctx().insert_or_assign<delta_time>({ dt.count() });

    float alpha = m_accumulator / m_fixed_step;
    m_state.ctx().insert_or_assign<scn::update_alpha>({ alpha });

    run_graph(m_variable_graph);
}

bool scn::world::load_from_desc(const scn::level_desc& lvl, const ecs::system_factory& system_factory)
{

    m_fixed_graph = m_fixed_organizer.graph();
	m_variable_graph = m_organizer.graph();
	return false;
}