#include <snk/scene/level/scn_level.h>
#include <snk/common/eng_profiler.h>

scn::level::level(std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(hot_manager)
	, m_owned(std::make_unique<scn::context_set>("level", std::move(hot_manager)))
	, m_set(m_owned.get())
{
}

scn::level::level(scn::context_set& set, std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(std::move(hot_manager))
	, m_set(&set)
{
}

scn::level::~level() = default;

void scn::level::update(std::chrono::duration<float> dt)
{
	m_set->update(dt);

	if (m_hot_reload_manager) {
		m_hot_reload_manager->tick();
	}
}
