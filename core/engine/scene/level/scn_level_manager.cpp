#include "scn_level_manager.h"
#include "scn_level_desc.h"

void scn::level_manager::update(std::chrono::duration<float> dt)
{
	active_lvl.update(dt);
}

bool scn::level_manager::load(const res::tag& level_tag)
{
	auto level_res = m_resource.require_sync<scn::level_desc>(level_tag);
	if (level_res.is_ready()) {
		active_lvl.clear();
		active_lvl.load_from_desc(*level_res, m_system_factory, m_assembler);
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
