#pragma once
#pragma once
#include "desc_system.h"
#include "desc_base.hpp"
#include "entt/entt.hpp"
#include "resources/res_resource_handle.hpp"
#include "res_tag.h"
#include <boost/json.hpp>
#include <string>
#include <functional>
#include <unordered_map>

namespace scn {
	struct tracked_component_info {
		std::string type_name;
		res::tag source_desc;
		boost::json::object local_overrides;
	};

	struct component_tracker {
		std::unordered_map<std::string, tracked_component_info> tracked_components;
	};

	class ecs_assembler {
	public:
		using desc_spawner_fn = std::function<void(
			entt::registry&,
			entt::entity,
			const desc::desc_base&,
			const std::string_view
			)>;

		using desc_patcher_fn = std::function<void(
			entt::registry&,
			entt::entity,
			const desc::desc_base&
			)>;

		struct spawner_pair {
			desc_spawner_fn spawn;
			desc_patcher_fn patch;
		};

		ecs_assembler(desc::desc_system& desc_sys)
			: m_desc_sys(desc_sys)
		{}

		~ecs_assembler() = default;
		ecs_assembler(const ecs_assembler&) = delete;
		ecs_assembler& operator=(const ecs_assembler&) = delete;
		ecs_assembler(ecs_assembler&&) = default;
		ecs_assembler& operator=(ecs_assembler&&) = default;

		void register_desc_spawner(const std::string_view desc_type, desc_spawner_fn spawner, desc_patcher_fn patcher = nullptr) {
			m_spawners[std::string{ desc_type }] = { std::move(spawner), std::move(patcher) };
		}

		void unregister_desc_spawner(const std::string_view desc_type) {
			m_spawners.erase(std::string{ desc_type });
		}

		desc::desc_system& get_desc_system() const { return m_desc_sys; }

		void spawn_from_desc(entt::registry& reg, entt::entity e, const desc::desc_base& desc, const std::string_view name = "") const
		{
			auto it = m_spawners.find(std::string{ desc.get_type() });
			if (it != m_spawners.end() && it->second.spawn) {
				it->second.spawn(reg, e, desc, name);
			} else {
				// egLOG("assembler/warning", "No spawner registered for desc type: {0}", desc.get_type());
			}
		}

		void assemble_and_apply(entt::registry& reg,
								entt::entity e,
								std::string actual_type,
								const res::res_handle<desc::desc_base>& parent_desc,
								const boost::json::object& overrides,
								const std::string_view name,
								bool is_hot_reload = false) const;

		void trigger_hot_reload_for_component(entt::registry& reg, const res::tag& changed_desc_tag) const;

	private:
		desc::desc_system& m_desc_sys;
		std::unordered_map<std::string, spawner_pair> m_spawners;
	};
}