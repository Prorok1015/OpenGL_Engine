#pragma once
#include <entt/fwd.hpp>
#include <cstdint>
#include <vector>
#include <unordered_map>
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
		// Registries currently attached. For tests and diagnostics.
		std::size_t attached_count() const { return m_registries.size(); }
		// Tags currently watched in resource_system. For tests and diagnostics.
		std::size_t watched_count() const { return m_watch_refs.size(); }

		void tick();

		void watch_resource(const res::tag& tag, res::resource_system::reload_callback&& callback);
		void unwatch_resource(const res::tag& tag);

	private:
		void on_tracker_changed(entt::registry& reg, entt::entity e);
		void on_prefab_changed(entt::registry& reg, entt::entity e);

		void track_tag(entt::registry& reg, const res::tag& tag);
		void retain(const res::tag& tag);
		void release(const res::tag& tag);

		ecs_assembler& m_assembler;
		res::resource_system& m_res_sys;

		std::unordered_set<entt::registry*> m_registries;

		// Per tag: the attached registries whose tag set holds it, plus 1 when it has an external callback.
		// The tag is watched in resource_system while its count is above zero.
		std::unordered_map<res::tag, std::uint32_t> m_watch_refs;
		std::unordered_map<entt::registry*, std::unordered_set<res::tag>> m_registry_tags;
		std::unordered_map<res::tag, res::resource_system::reload_callback> m_external_watched_tags;

		// Filled by the watcher callbacks, which resource_system delivers on the main thread; drained by tick().
		std::vector<res::tag> m_reload_queue;
	};
}