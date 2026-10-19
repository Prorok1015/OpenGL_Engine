#pragma once
#include <entt/fwd.hpp>
#include <vector>
#include <unordered_set>
#include <snk/resource/res_tag.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/snk_api.h>

namespace core::res { class resource_system; }

namespace scn {

	class SNK_API hot_reload_manager {
	public:
		hot_reload_manager(ecs_assembler& assembler, res::resource_system& res_sys);
		~hot_reload_manager();

		void attach_registry(entt::registry& reg);
		void detach_registry(entt::registry& reg);

		void tick();

		void watch_resource(const res::tag& tag, res::resource_system::reload_callback&& callback) {
			ensure_watched(tag);
			m_external_watched_tags[tag] = std::move(callback);
		}

		void unwatch_resource(const res::tag& tag) {
			m_external_watched_tags.erase(tag);
		}

	private:
		void on_tracker_changed(entt::registry& reg, entt::entity e);
		void on_prefab_changed(entt::registry& reg, entt::entity e);

		void ensure_watched(const res::tag& tag);

		ecs_assembler& m_assembler;
		res::resource_system& m_res_sys;

		std::unordered_set<entt::registry*> m_registries;

		std::unordered_set<res::tag> m_watched_tags;
		std::unordered_map<res::tag, res::resource_system::reload_callback> m_external_watched_tags;

		// Filled by the watcher callbacks, which resource_system delivers on the main thread; drained by tick().
		std::vector<res::tag> m_reload_queue;
	};
}