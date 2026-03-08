#pragma once
#include <entt/entt.hpp>
#include <mutex>
#include <vector>
#include <unordered_set>
#include "res_tag.h"
#include "scn_ecs_assembler.h"

namespace core::res { class resource_system; }

namespace scn {

	class hot_reload_manager {
	public:
		hot_reload_manager(ecs_assembler& assembler, res::resource_system& res_sys);
		~hot_reload_manager();

		void attach_registry(entt::registry& reg);
		void detach_registry(entt::registry& reg);

		void tick();

	private:
		void on_tracker_changed(entt::registry& reg, entt::entity e);
		void on_prefab_changed(entt::registry& reg, entt::entity e);

		void ensure_watched(const res::tag& tag);

		ecs_assembler& m_assembler;
		res::resource_system& m_res_sys;

		std::unordered_set<entt::registry*> m_registries;

		std::unordered_set<res::tag> m_watched_tags;

		std::mutex m_queue_mtx;
		std::vector<res::tag> m_reload_queue;
	};
}