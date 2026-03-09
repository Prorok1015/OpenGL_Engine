#include "scn_hot_reload_manager.h"
#include "res_system.h" 
#include "scn_prefab_desc.h"

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

		for (const auto& tag : m_watched_tags) {
			m_res_sys.unwatch(tag, this);
		}
	}

	void hot_reload_manager::attach_registry(entt::registry& reg)
	{
		if (m_registries.insert(&reg).second) {
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
		}
	}

	void hot_reload_manager::ensure_watched(const res::tag& tag)
	{
		if (!tag.is_valid()) return;

		if (m_watched_tags.insert(tag).second) {
			m_res_sys.watch(tag, this, [this] (const res::tag& changed_tag) {
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				if (std::find(m_reload_queue.begin(), m_reload_queue.end(), changed_tag) == m_reload_queue.end()) {
					m_reload_queue.push_back(changed_tag);
				}
			});
		}
	}

	void hot_reload_manager::on_tracker_changed(entt::registry& reg, entt::entity e)
	{
		const auto& tracker = reg.get<scn::component_tracker>(e);
		for (const auto& [comp_name, info] : tracker.tracked_components) {
			ensure_watched(info.source_desc);
		}
	}

	void hot_reload_manager::on_prefab_changed(entt::registry& reg, entt::entity e)
	{
		const auto& instance = reg.get<scn::prefab_instance>(e);
		ensure_watched(instance.source_prefab);
	}

	void hot_reload_manager::tick()
	{
		std::vector<res::tag> tags_to_reload;

		{
			std::lock_guard<std::mutex> lock(m_queue_mtx);
			tags_to_reload = std::move(m_reload_queue);
			m_reload_queue.clear();
		}

		if (tags_to_reload.empty() || m_registries.empty()) return;

		for (const auto& tag : tags_to_reload) {
			auto changed_desc_handle = m_res_sys.require_sync<desc::desc_base>(tag);

			if (changed_desc_handle.has_error()) {
				continue;
			}

			if (m_external_watched_tags.contains(tag)) {
				m_external_watched_tags.at(tag)(tag);
			}

			for (auto* reg_ptr : m_registries) {
				m_assembler.trigger_hot_reload_for_component(*reg_ptr, changed_desc_handle);
				scn::trigger_hot_reload(m_assembler, *reg_ptr, changed_desc_handle);
			}
		}
	}
}