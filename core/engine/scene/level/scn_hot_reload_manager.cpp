#include <snk/scene/level/scn_hot_reload_manager.h>
#include <entt/entt.hpp>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_prefab_desc.h>

namespace scn {

	hot_reload_manager::hot_reload_manager(ecs_assembler& assembler, res::resource_system& res_sys)
		: m_assembler(assembler), m_res_sys(res_sys)
	{
	}

	hot_reload_manager::~hot_reload_manager()
	{
		for (auto* reg : m_registries) {
			reg->on_construct<scn::component_tracker>().disconnect(this);
			reg->on_update<scn::component_tracker>().disconnect(this);
			reg->on_construct<scn::prefab_instance>().disconnect(this);
			reg->on_update<scn::prefab_instance>().disconnect(this);
		}

		for (const auto& entry : m_watch_refs) {
			m_res_sys.unwatch(entry.first, this);
		}
	}

	void hot_reload_manager::attach_registry(entt::registry& reg)
	{
		if (m_registries.insert(&reg).second) {
			m_registry_tags.try_emplace(&reg);

			reg.on_construct<scn::component_tracker>().connect<&hot_reload_manager::on_tracker_changed>(this);
			reg.on_update<scn::component_tracker>().connect<&hot_reload_manager::on_tracker_changed>(this);

			reg.on_construct<scn::prefab_instance>().connect<&hot_reload_manager::on_prefab_changed>(this);
			reg.on_update<scn::prefab_instance>().connect<&hot_reload_manager::on_prefab_changed>(this);
		}
	}

	void hot_reload_manager::detach_registry(entt::registry& reg)
	{
		if (m_registries.erase(&reg)) {
			reg.on_construct<scn::component_tracker>().disconnect(this);
			reg.on_update<scn::component_tracker>().disconnect(this);
			reg.on_construct<scn::prefab_instance>().disconnect(this);
			reg.on_update<scn::prefab_instance>().disconnect(this);

			if (auto node = m_registry_tags.extract(&reg)) {
				for (const auto& tag : node.mapped()) {
					release(tag);
				}
			}
		}
	}

	void hot_reload_manager::watch_resource(const res::tag& tag, res::resource_system::reload_callback&& callback)
	{
		auto [it, inserted] = m_external_watched_tags.try_emplace(tag);
		if (inserted) {
			retain(tag);
		}
		it->second = std::move(callback);
	}

	void hot_reload_manager::unwatch_resource(const res::tag& tag)
	{
		if (m_external_watched_tags.erase(tag)) {
			release(tag);
		}
	}

	void hot_reload_manager::retain(const res::tag& tag)
	{
		if (!tag.is_valid()) return;

		if (++m_watch_refs[tag] == 1) {
			m_res_sys.watch(tag, this, [this] (const res::tag& changed_tag) {
				if (std::find(m_reload_queue.begin(), m_reload_queue.end(), changed_tag) == m_reload_queue.end()) {
					m_reload_queue.push_back(changed_tag);
				}
			});
		}
	}

	void hot_reload_manager::release(const res::tag& tag)
	{
		const auto it = m_watch_refs.find(tag);
		if (it == m_watch_refs.end()) return;

		if (--it->second == 0) {
			m_watch_refs.erase(it);
			m_res_sys.unwatch(tag, this);
		}
	}

	void hot_reload_manager::track_tag(entt::registry& reg, const res::tag& tag)
	{
		if (!tag.is_valid()) return;

		const auto it = m_registry_tags.find(&reg);
		if (it == m_registry_tags.end()) return;

		if (it->second.insert(tag).second) {
			retain(tag);
		}
	}

	void hot_reload_manager::on_tracker_changed(entt::registry& reg, entt::entity e)
	{
		const auto& tracker = reg.get<scn::component_tracker>(e);
		for (const auto& [comp_name, info] : tracker.tracked_components) {
			track_tag(reg, info.source_desc);
		}
	}

	void hot_reload_manager::on_prefab_changed(entt::registry& reg, entt::entity e)
	{
		const auto& instance = reg.get<scn::prefab_instance>(e);
		track_tag(reg, instance.source_prefab);
	}

	void hot_reload_manager::tick()
	{
		std::vector<res::tag> tags_to_reload = std::move(m_reload_queue);
		m_reload_queue.clear();

		if (tags_to_reload.empty()) return;

		for (const auto& tag : tags_to_reload) {
			// Its last watcher left after the change was queued (or an earlier callback of this tick released it).
			if (!m_watch_refs.contains(tag)) continue;

			// External callbacks (e.g. context_manager level reload) handle their own typed load.
			if (const auto ext = m_external_watched_tags.find(tag); ext != m_external_watched_tags.end()) {
				// A copy: the callback may unwatch (and re-watch) its own tag, destroying the stored one.
				const auto callback = ext->second;
				callback(tag);
				continue;
			}

			// A level without worlds has no registry, but its own reload callback above must still run.
			if (m_registries.empty()) continue;

			// Internal component-tracker hot-reload. A desc whose __parent changed arrives under its own tag, after the
			// parent: resource_system follows the dependency graph. desc_base shares one cache block with the concrete desc type.
			// Deleted or moved away (a branch switch, a save that renames a backup): keep what is loaded.
			if (!m_res_sys.exists(tag)) {
				egLOG_DEBUG("scene/hot_reload", "hot reload: '{}' is gone, keeping previous state", tag.view());
				continue;
			}
			auto changed_desc_handle = m_res_sys.require_sync<desc::desc_base>(tag);
			if (!ENGINE_VERIFY(changed_desc_handle.is_ready(), "hot reload: '{}' failed to reload, keeping previous state", tag.view())) {
				continue;
			}

			for (auto* reg_ptr : m_registries) {
				m_assembler.trigger_hot_reload_for_component(*reg_ptr, changed_desc_handle);
				scn::trigger_hot_reload(m_assembler, *reg_ptr, changed_desc_handle);
			}
		}
	}
}