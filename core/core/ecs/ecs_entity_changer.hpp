#pragma once
#include <entt/entity/registry.hpp>
#include <unordered_map>
#include <vector>
namespace ecs {

    struct batch_interface {
        virtual ~batch_interface() = default;
        virtual void apply(entt::registry& reg) = 0;
    };

    template <typename T>
    struct emplace_batch : batch_interface {
        std::unordered_map<entt::entity, T> data;

        void apply(entt::registry& reg) override {
            for (auto&& [ent, component] : data) {
                if (reg.valid(ent)) {
                    reg.emplace_or_replace<T>(ent, std::move(component));
                }
            }
        }
    };

    template <typename T>
    struct remove_batch : batch_interface {
        std::vector<entt::entity> entities;

        void apply(entt::registry& reg) override {
            for (auto ent : entities) {
                if (reg.valid(ent)) reg.remove<T>(ent);
            }
        }
    };

    class entity_changer {
    public:
        template <typename T, typename... Args>
        void emplace(entt::entity ent, Args&&... args) {
            get_batch<T, emplace_batch<T>>().data[ent] = T{ std::forward<Args>(args)... };
        }

        template <typename T>
        void remove(entt::entity ent) {
            get_batch<T, remove_batch<T>>().entities.push_back(ent);
        }

        void destroy(entt::entity ent) {
            m_to_destroy.push_back(ent);
        }

        void apply(entt::registry& reg) {
            for (auto& [id, batch] : m_batches) batch->apply(reg);
            for (auto ent : m_to_destroy) if (reg.valid(ent)) reg.destroy(ent);
            clear();
        }

        void clear() {
            m_batches.clear();
            m_to_destroy.clear();
        }

    private:
        template<typename T, typename B> 
        B& get_batch() {
            auto tid = entt::type_hash<T>::value();
            if (!m_batches.contains(tid)) m_batches[tid] = std::make_unique<B>();
            return static_cast<B&>(*m_batches[tid]);
        }

        std::unordered_map<entt::id_type, std::unique_ptr<batch_interface>> m_batches;
        std::vector<entt::entity> m_to_destroy;
    };

} // namespace ecs