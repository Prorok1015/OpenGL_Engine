#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>
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

	class entity_spawner : public sandbox_registry
	{
	};

    // What entt::basic_continuous_loader::get reads, served from a sandbox storage. Every archive(entity) is the next
    // packed slot, from 0 up: the loader asks for an entity once per element, then for its value only when the type
    // has one, so the entity read is what advances (#708). The entity storage's live ids are its first free_list()
    // slots; the rest are ids the sandbox released, which the loader reads after `in_use` and must not restore.
    template<typename Storage>
    struct sandbox_loader_archive {
        static constexpr bool is_entity_storage = std::is_same_v<typename Storage::element_type, sandbox_entity>;

        entt::continuous_loader& loader;
        const Storage& storage;
        std::size_t next = 0;
        sandbox_entity current = entt::null;
        int counts_read = 0;

        sandbox_loader_archive(entt::continuous_loader& l, const Storage& s)
            : loader(l), storage(s) {
        }

        void operator()(std::size_t& len) {
            len = storage.size();
        }

        // The element count, then, for the entity storage only, how many of them are live.
        template<typename Type>
            requires (std::is_same_v<Type, typename entt::entt_traits<entt::entity>::entity_type> && !std::is_same_v<Type, std::size_t>)
        void operator()(Type& count) {
            if constexpr (is_entity_storage) {
                count = static_cast<Type>(counts_read++ == 0 ? storage.size() : storage.free_list());
            } else {
                count = static_cast<Type>(storage.size());
            }
        }

        void operator()(entt::entity& ent) {
            if (next < storage.size()) {
                current = storage.data()[next++];
                ent = static_cast<entt::entity>(static_cast<std::uint32_t>(current));
            } else {
                current = entt::null;
                ent = entt::null;
            }
        }

        template<typename T>
        void operator()(T& out_component) {
            if constexpr (std::is_same_v<std::remove_cvref_t<T>, entt::entity>) {
                out_component = entt::null;
            } else if (current != entt::null) {
                out_component = storage.get(current);
                remap_fields(out_component);
            }
        }

    private:
        template<typename T>
        void remap_fields(T& component) {
            using RawT = std::remove_cvref_t<T>;
            if constexpr (std::is_fundamental_v<RawT> || std::is_enum_v<RawT>) return;

            auto type = entt::resolve<RawT>();
            if (type) {
                // get() yields the field's value (a copy under the default policy), so the mapped id is written
                // back through set(); the field's meta type holds the value, not a pointer to it.
                for (auto&& [id, data] : type.data()) {
                    if (data.type() == entt::resolve<entt::entity>()) {
                        const entt::meta_any value = data.get(component);
                        if (const auto* raw = value.template try_cast<entt::entity>()) {
                            data.set(component, loader.map(*raw));
                        }
                    } else if (data.type() == entt::resolve<sandbox_entity>()) {
                        const entt::meta_any value = data.get(component);
                        if (const auto* raw = value.template try_cast<sandbox_entity>()) {
                            data.set(component, static_cast<sandbox_entity>(loader.map(static_cast<entt::entity>(*raw))));
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