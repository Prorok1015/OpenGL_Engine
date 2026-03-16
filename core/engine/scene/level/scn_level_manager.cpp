#include "scn_level_manager.h"
#include "scn_level_desc.h"

scn::level_manager::~level_manager()
{}

void scn::level_manager::update(std::chrono::duration<float> dt)
{
	active_lvl.update(dt);
}

bool scn::level_manager::load(const res::tag& level_tag)
{
	auto level_res = m_resource.require_sync<scn::level_desc>(level_tag);
	if (level_res.is_ready()) {
		if (m_hot_reload_manager && active_lvl.get_tag().is_valid()) {
			m_hot_reload_manager->unwatch_resource(active_lvl.get_tag());
		}
		active_lvl.clear();
		active_lvl.load_from_desc(*level_res, m_system_factory, m_assembler);
		if (m_hot_reload_manager) {
			m_hot_reload_manager->watch_resource(level_tag, [this](const res::tag& changed_tag) {
				auto level_res = m_resource.require_sync<scn::level_desc>(changed_tag);
				if (level_res.is_ready()) {
					bool is_full_reload = false;
					
					if (level_res->get_worlds().size() != active_lvl.get_world_count()) {
						is_full_reload = true;
					} else {
						std::size_t world_index = 0;
						for (const auto& world_desc : level_res->get_worlds()) {
							auto& world = active_lvl.get_world(world_index++);
							if (world.get_name() != world_desc->get_name()) {
								is_full_reload = true;
								break;
							}
						}
					}

					if (is_full_reload) {
						active_lvl.clear();
						active_lvl.load_from_desc(*level_res, m_system_factory, m_assembler);
					} else {
						active_lvl.load_systems_from_desc(*level_res, m_system_factory);
					}
				} else {
					egLOG("Level Manager", "Failed to reload level '{0}'", changed_tag.view());
				}
			});
		}
	} else {
		egLOG("Level Manager", "Failed to load level '{}'", level_tag.view());
		return false;
	}
	return true;
}

void scn::level_manager::load_world(const res::tag& world_tag)
{
	auto world_res = m_resource.require_sync<scn::world_desc>(world_tag);
	if (world_res.is_ready()) {
		active_lvl.clear();
		active_lvl.load_world_from_desc(*world_res, m_system_factory, m_assembler);
	} else {
		egLOG("Level Manager", "Failed to load level '{}'", world_tag.view());
	}
}