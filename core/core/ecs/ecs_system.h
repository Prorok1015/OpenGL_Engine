#pragma once
#include "common.h"
#include "logger/engine_log.h"
#include "ecs_system_interface.hpp"
#include <entt/fwd.hpp>

namespace ecs
{
	class system_factory
	{
		using initializer_cb = std::function<std::unique_ptr<system_interface>(entt::registry&, entt::organizer&)>;
	public:
		system_factory() = default;
		~system_factory() = default;
		system_factory(const system_factory&) = delete;
		system_factory(system_factory&&) = delete;
		system_factory& operator=(const system_factory&) = delete;
		system_factory& operator=(system_factory&&) = delete;

		void register_system(const std::string& name, std::function<void(entt::registry&, entt::organizer&)> func)
		{
			ASSERT_MSG(!m_systems.contains(name), "ecs system already contains in the factory!");
			m_systems[name] = [func = std::move(func)](entt::registry& reg, entt::organizer& org) -> std::unique_ptr<system_interface> {
				func(reg, org);
				return nullptr;
			};
		}

		template<typename T, typename... ARGS>
		void register_system_class(const std::string& name, ARGS... args)
		{
			ASSERT_MSG(!m_systems.contains(name), "ecs system already contains in the factory!");
			m_systems[name] = [args...](entt::registry& reg, entt::organizer& org) {
				auto sys = std::make_unique<T>(args...);
				sys->register_in_world(reg, org);
				return sys;
			};
		}

		void unregister_system(const std::string_view name)
		{
			m_systems.erase(std::string{ name });
		}

		std::unique_ptr<system_interface> create_system(const std::string& name, entt::registry& registry, entt::organizer& organizer) const
		{
			auto it = m_systems.find(name);
			if (it != m_systems.end()) {
				return it->second(registry, organizer);
			}

			egLOG("ecs/systems/create", "required system isn't registred '{0}'", name);
			return nullptr;
		}

	private:
		std::unordered_map<std::string, initializer_cb> m_systems;
	};
}