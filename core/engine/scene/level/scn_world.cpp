#include "scn_world.h"
#include "scn_model.h"

void scn::world::update(std::chrono::duration<float> dt)
{
	m_state.ctx().insert_or_assign<scn::delta_time>(scn::delta_time{ dt.count() });

	auto graph = m_organizer.graph();
	for (auto&& system : graph) {
		system.prepare(m_state);
		system.callback()(system.data(), m_state);
	}

}