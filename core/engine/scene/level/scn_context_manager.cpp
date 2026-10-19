#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>

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

scn::context_manager::context_manager(res::resource_system& resource,
									  scn::ecs_assembler& assembler,
									  ecs::system_factory& system_factory,
									  std::shared_ptr<scn::hot_reload_manager> hot_manager)
	: m_hot_reload_manager(hot_manager)
	, m_assembler(assembler)
	, m_system_factory(system_factory)
	, m_resource(resource)
	, m_level(add_level_set(), hot_manager)
{}

scn::context_manager::~context_manager() = default;

scn::context_set& scn::context_manager::add_level_set()
{
	return *m_sets.emplace_back(std::make_unique<scn::context_set>(std::string{ level_set_name }, m_hot_reload_manager));
}

void scn::context_manager::update(std::chrono::duration<float> dt)
{
	for (const auto& set : m_sets) {
		if (set->active()) set->update(dt);
	}

	if (m_hot_reload_manager) {
		m_hot_reload_manager->tick();
	}
}

scn::context_set& scn::context_manager::create_set(std::string_view name)
{
	if (auto* existing = find_set(name)) {
		egLOG_ERROR("scene", "create_set: set '{}' already exists, keeping the existing one", name);
		return *existing;
	}
	return *m_sets.emplace_back(std::make_unique<scn::context_set>(std::string{ name }, m_hot_reload_manager));
}

void scn::context_manager::destroy_set(std::string_view name)
{
	ASSERT_MSG(name != level_set_name, "destroy_set: the level set lives as long as the manager");
	const auto it = std::ranges::find_if(m_sets, [name](const auto& set) { return set->name() == name; });
	ASSERT_MSG(it != m_sets.end(), "destroy_set: no set with this name");
	m_sets.erase(it);
}

scn::context_set* scn::context_manager::find_set(std::string_view name) const
{
	const auto it = std::ranges::find_if(m_sets, [name](const auto& set) { return set->name() == name; });
	return it == m_sets.end() ? nullptr : it->get();
}

scn::context_set& scn::context_manager::get_set(std::string_view name) const
{
	auto* set = find_set(name);
	ASSERT_MSG(set, "context_manager: no set with this name");
	return *set;
}

scn::world_context& scn::context_manager::create_context(std::string_view set, std::string_view name, context_kind kind,
														 const std::vector<std::string>& systems)
{
	auto& target = get_set(set);
	auto& context = target.create_context(name, target.acquire_free_id(), kind);
	target.add_systems(context, systems, m_system_factory);
	return context;
}

void scn::context_manager::destroy_context(std::string_view set, std::string_view name)
{
	get_set(set).destroy_context(name);
}

scn::world_context* scn::context_manager::find(std::string_view set, std::string_view name) const
{
	const auto* target = find_set(set);
	return target ? target->find(name) : nullptr;
}

bool scn::context_manager::load_level(const res::tag& level_tag)
{
	auto level_res = m_resource.require_sync<scn::level_desc>(level_tag);
	if (level_res.is_ready()) {
		auto& set = get_set(level_set_name);
		if (m_hot_reload_manager && set.tag().is_valid()) {
			m_hot_reload_manager->unwatch_resource(set.tag());
		}
		set.clear();
		set.load_from_desc(*level_res, m_system_factory, m_assembler);
		if (m_hot_reload_manager) {
			m_hot_reload_manager->watch_resource(level_tag, [this](const res::tag& changed_tag) {
				auto level_res = m_resource.require_sync<scn::level_desc>(changed_tag);
				if (level_res.is_ready()) {
					auto& set = get_set(level_set_name);
					std::vector<std::string> new_names;
					const bool names_ok = collect_world_names(*level_res, new_names);
					if (!names_ok) {
						// load_from_desc dereferences every world handle, so a broken world file must not reach it
						egLOG_WARN("Level Manager", "level reload '{}': world desc failed to load, keeping the current level", changed_tag.view());
						return;
					}
					// compare against the desc's worlds only (in desc order, which decides context ids): runtime-created contexts are not part of it
					const std::vector<std::string> old_names = set.desc_world_names();
					const bool is_full_reload = new_names != old_names;

					if (is_full_reload) {
						// drop only the level contexts; runtime-created ones keep their state
						for (const auto& name : old_names) {
							if (set.has(name)) set.destroy_context(name);
						}
						// a runtime context sharing a name with a new desc world is replaced by the desc one
						for (const auto& name : new_names) {
							if (set.has(name)) set.destroy_context(name);
						}
						set.load_from_desc(*level_res, m_system_factory, m_assembler);
					} else {
						set.load_systems_from_desc(*level_res, m_system_factory);
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
