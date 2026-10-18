#include "scn_level_manager.h"
#include "scn_level_desc.h"

namespace {
	// Names in desc order; false when a world desc failed to load.
	bool collect_world_names(const scn::level_desc& desc, std::vector<std::string>& out)
	{
		bool ok = true;
		for (const auto& world_desc : desc.get_worlds()) {
			const auto desc_ptr = world_desc.get_sync();
			if (!desc_ptr) {
				ok = false;
				continue;
			}
			out.emplace_back(desc_ptr->get_name());
		}
		return ok;
	}
}

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
		m_desc_world_names.clear();
		collect_world_names(*level_res, m_desc_world_names);
		if (m_hot_reload_manager) {
			m_hot_reload_manager->watch_resource(level_tag, [this](const res::tag& changed_tag) {
				auto level_res = m_resource.require_sync<scn::level_desc>(changed_tag);
				if (level_res.is_ready()) {
					std::vector<std::string> new_names;
					const bool names_ok = collect_world_names(*level_res, new_names);
					if (!names_ok) {
						// load_from_desc dereferences every world handle, so a broken world file must not reach it
						egLOG_WARN("Level Manager", "level reload '{}': world desc failed to load, keeping the current level", changed_tag.view());
						return;
					}
					// compare against the desc's worlds only: runtime-created worlds are not part of it
					const bool is_full_reload = new_names != m_desc_world_names;

					if (is_full_reload) {
						// drop only the desc-owned worlds; runtime-created ones keep their state
						for (const auto& name : m_desc_world_names) {
							if (active_lvl.has_world(name)) active_lvl.remove_world(name);
						}
						// a runtime world sharing a name with a new desc world is replaced by the desc one
						for (const auto& name : new_names) {
							if (active_lvl.has_world(name)) active_lvl.remove_world(name);
						}
						active_lvl.state().clear();
						active_lvl.load_from_desc(*level_res, m_system_factory, m_assembler);
						m_desc_world_names = std::move(new_names);
					} else {
						active_lvl.load_systems_from_desc(*level_res, m_system_factory);
					}
					if (m_on_reload) m_on_reload();
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
