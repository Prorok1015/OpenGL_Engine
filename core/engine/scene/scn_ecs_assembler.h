#pragma once
#include "desc_system.h"
#include "desc_base.hpp"
#include "entt/entt.hpp"
#include "resources/res_resource_handle.hpp"
#include <string>
#include <functional>
#include <unordered_map>

namespace scn {
	class ecs_assembler {
	public:
        using desc_spawner_fn = std::function<void(
            entt::registry&,
            entt::entity,
            const desc::desc_base&,
            const std::string_view
            )>;

		ecs_assembler(desc::desc_system& desc_sys)
			: m_desc_sys(desc_sys)
		{}

		~ecs_assembler() = default;
		ecs_assembler(const ecs_assembler&) = delete;
		ecs_assembler& operator=(const ecs_assembler&) = delete;
		ecs_assembler(ecs_assembler&&) = default;
		ecs_assembler& operator=(ecs_assembler&&) = default;

        void register_desc_spawner(const std::string_view desc_type, desc_spawner_fn spawner) {
			m_spawners[std::string{ desc_type }] = spawner;
        }

		void unregister_desc_spawner(const std::string_view desc_type) {
			m_spawners.erase(std::string{ desc_type });
        }

		void spawn_from_desc(entt::registry& reg, entt::entity e, const desc::desc_base& desc, const std::string_view name = "") const
		{
			auto it = m_spawners.find(std::string{ desc.get_type() });
			if (it != m_spawners.end()) {
				it->second(reg, e, desc, name);
			} else {
				egLOG("assembler/warning", "No spawner registered for desc type: {0}", desc.get_type());
			}
		}

        void assemble_and_apply(entt::registry& reg,
								entt::entity e,
								std::string actual_type,
								const res::res_handle<desc::desc_base>& parent_desc,
								const boost::json::object& overrides,
								const std::string_view name) const;
		

    private:
        desc::desc_system& m_desc_sys;
		std::unordered_map<std::string, desc_spawner_fn> m_spawners;
	};
}