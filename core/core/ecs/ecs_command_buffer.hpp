#pragma once
#include "ecs_entity_changer.hpp"
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>
#include <entt/meta/resolve.hpp>
namespace ecs
{
	enum class sandbox_entity : std::uint32_t {};
	using sandbox_registry = entt::basic_registry<sandbox_entity>;
	using sandbox_snapshot = entt::basic_snapshot<sandbox_registry>;
	using sandbox_continuos_loader = entt::basic_continuous_loader<sandbox_registry>;
	using sandbox_snapshot_loader = entt::basic_snapshot_loader<sandbox_registry>;

	class command_buffer
	{
	public:
		sandbox_registry& get_sandbox() { return sandbox; }
	private:
		sandbox_registry sandbox;

	};

	class entity_spawner : public sandbox_registry
	{
	};

    template<typename Storage>
    struct sandbox_loader_archive {
        entt::continuous_loader& loader;
        const Storage& storage;
        typename Storage::const_iterator it;

        sandbox_loader_archive(entt::continuous_loader& l, const Storage& s)
            : loader(l), storage(s), it(s.cbegin()) {
        }

        void operator()(std::size_t& len) {
            len = storage.size();
        }

        template<typename Type>
            requires (std::is_same_v<Type, typename entt::entt_traits<entt::entity>::entity_type> && !std::is_same_v<Type, std::size_t>)
        void operator()(Type& in_use) {
            in_use = static_cast<Type>(storage.size());
        }

        void operator()(entt::entity& ent) {
            if (it != storage.end()) {
                ent = static_cast<entt::entity>(static_cast<uint32_t>(storage.data()[it.index()]));
            } else {
                ent = entt::null;
            }
        }

        template<typename T>
        void operator()(T& out_component) {
            if (it != storage.end()) {
                out_component = *it;
                remap_fields(out_component);
                ++it;
            }
            else if constexpr (std::is_same_v<std::remove_cvref_t<T>, entt::entity>) {
                out_component = entt::null;
            }
        }

    private:
        template<typename T>
        void remap_fields(T& component) {
            using RawT = std::remove_cvref_t<T>;
            if constexpr (std::is_fundamental_v<RawT> || std::is_enum_v<RawT>) return;

            auto type = entt::resolve<RawT>();
            if (type) {
                for (auto&& [id, data] : type.data()) {
                    if (data.type() == entt::resolve<entt::entity>()) {
                        if (auto* ptr = data.get(component).template cast<entt::entity*>()) {
                            *ptr = loader.map(*ptr);
                        }
                    } else if (data.type() == entt::resolve<sandbox_entity>()) {
                        if (auto* ptr = data.get(component).template cast<sandbox_entity*>()) {
                            auto raw = static_cast<entt::entity>(*ptr);
                            *ptr = static_cast<sandbox_entity>(loader.map(raw));
                        }
                    }
                }
            }
        }
    };

    struct meta_archive {
        entt::continuous_loader& loader;

        template<typename... Args>
        void operator()(Args&&... args) {
            (process(std::forward<Args>(args)), ...);
        }

    private:
        template<typename T>
        void process(T&& value) {
            using RawT = std::remove_cvref_t<T>;

            if constexpr (std::is_same_v<RawT, entt::entity> || std::is_same_v<RawT, sandbox_entity>) {
                if constexpr (!std::is_const_v<std::remove_reference_t<T>>) {
                    if constexpr (std::is_same_v<RawT, entt::entity>) {
                        value = loader.map(value);
                    }
                    else {
                        auto raw = static_cast<entt::entity>(value);
                        value = static_cast<sandbox_entity>(loader.map(raw));
                    }
                }
            }
            else if constexpr (!std::is_fundamental_v<RawT> && !std::is_enum_v<RawT>) {
                auto type = entt::resolve<RawT>();
                if (type) {
                    for (auto&& [id, data] : type.data()) {
                        if (data.type() == entt::resolve<entt::entity>()) {
                            if (auto* ptr = data.get(value).template cast<entt::entity*>()) {
                                *ptr = loader.map(*ptr);
                            }
                        }
                        else if (data.type() == entt::resolve<sandbox_entity>()) {
                            if (auto* ptr = data.get(value).template cast<sandbox_entity*>()) {
                                auto raw_snd = static_cast<entt::entity>(*ptr);
                                *ptr = static_cast<sandbox_entity>(loader.map(raw_snd));
                            }
                        }
                    }
                }
            }
        }
    };
}