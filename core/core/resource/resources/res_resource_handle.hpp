#pragma once
#include "resolvers/res_control_block.hpp"
#include "res_tag.h"
#include <memory>
#include <functional>

namespace core::res
{
    template<typename T>
    class res_handle {
    public:
		using resource_control_block = res_control_block<std::shared_ptr<T>>;

        struct hash
        {
            std::size_t operator()(const res_handle& h) const {
                return static_cast<std::size_t>(h.m_block.get());
            }
        };

        res_handle() = default;
        res_handle(std::shared_ptr<resource_control_block> block, const ::res::tag& tag)
            : m_block(std::move(block)), m_tag(tag) {
        }

        auto operator<=> (const res_handle<T>&) const noexcept = default;

        bool is_ready() const { return m_block && m_block->is_ready(); }
        bool has_error() const { return m_block && m_block->has_error(); }
		bool is_valid() const { return m_block != nullptr; }

        std::shared_ptr<T> get() const {
            return is_ready() ? m_block->data : nullptr;
        }

        std::shared_ptr<T> get_sync() const {
            return m_block->get();
        }

        void then(std::function<void(T&)> cb) {
            if (!m_block) return;
            m_block->then([cb](auto& block) {
                if (block.data) cb(*block.data);
            });
        }

        const ::res::tag& get_tag() const
        {
            return m_tag;
        }

        T& operator*() const {
			return *get_sync();
        }

		std::shared_ptr<T> operator->() const {
			return get_sync();
		}

        std::shared_ptr<resource_control_block> get_control_block() const {
            return m_block;
		}

    private:
        std::shared_ptr<resource_control_block> m_block;
        ::res::tag m_tag;
    };

}
