#pragma once
#include "res_control_block.hpp"
#include <memory>
#include <functional>

namespace core::res
{
    template<typename T>
    class res_handle {
    public:
		using resource_control_block = res_control_block<std::shared_ptr<T>>;

        res_handle() = default;
        res_handle(std::shared_ptr<resource_control_block> block)
            : m_block(std::move(block)) {
        }

        bool is_ready() const { return m_block && m_block->status == res_status::ready; }
        bool has_error() const { return m_block && m_block->status == res_status::error; }

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
            /*
            if (is_ready()) {
                cb(*m_block->data);
            } else {
                std::lock_guard lock(m_block->callback_mtx);
                m_block->on_ready_callbacks.push_back([cb](auto& block) {
                    if (block.data) cb(*block.data);
                });
            }*/
        }

    private:
        std::shared_ptr<resource_control_block> m_block;
    };
}