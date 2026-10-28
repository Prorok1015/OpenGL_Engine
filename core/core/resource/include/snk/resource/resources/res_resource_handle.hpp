#pragma once
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/resource/res_tag.h>
#include <snk/common/ds/ds_polymorphic_cast.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <chrono>
#include <memory>
#include <functional>

namespace core::res
{
    template<typename T>
    class res_handle {
    public:
		using resource_control_block = res_resource_block;

        res_handle() = default;
        ~res_handle() = default;
        res_handle(std::shared_ptr<resource_control_block> block, const ::res::tag& tag, ds::task_system* tasks = nullptr)
            : m_block(std::move(block)), m_tag(tag), m_tasks(tasks) {
        }
		res_handle(const res_handle<T>&) = default;
		res_handle(res_handle<T>&&) = default;
		res_handle<T>& operator= (const res_handle<T>&) = default;
		res_handle<T>& operator= (res_handle<T>&&) = default;

        auto operator<=> (const res_handle<T>&) const noexcept = default;

        bool is_ready() const { return m_block && m_block->is_ready(); }
        bool has_error() const { return m_block && m_block->has_error(); }
		bool is_valid() const { return m_block != nullptr; }

        std::shared_ptr<T> get() const {
            if (!m_block || !m_block->is_ready()) return nullptr;
            return ds::polymorphic_pointer_cast<T>(m_block->data);
        }

        // Blocks until the load finished, either way (check is_ready() / has_error() after).
        // Not for a task_system worker: adapters declare what they need (adapter_interface::collect_dependencies)
        // instead of waiting, and a worker that waits for a load queued behind it stalls the pool. Under
        // policy::manual the queued loads are run right here, since nothing runs in the background.
        void wait() const {
            if (!m_block) return;
            if (m_block->is_ready() || m_block->has_error()) return;
            if (m_tasks && m_tasks->mode() == ds::policy::manual) {
                // When no task is queued the block may still be waiting for a resolver thread, whose completion
                // queues the next one.
                while (!m_block->is_ready() && !m_block->has_error()) {
                    if (!m_tasks->run_one(ds::lane::compute) && !m_tasks->run_one(ds::lane::io)) {
                        m_block->wait_for(std::chrono::milliseconds(1));
                    }
                }
                return;
            }
            ASSERT_MSG(!(m_tasks && ds::task_system::on_worker()), "res_handle::wait on a pool thread: '{}' is not ready, declare it as a dependency instead", m_tag.view());
            m_block->wait();
        }

        std::shared_ptr<T> get_sync() const {
            if (!m_block) return nullptr;
            wait();
            if (m_block->has_error() || !m_block->data) {
                egLOG_ERROR("resource", "get_sync on failed resource '{}': {}", m_tag.view(), m_block->error_msg);
                return nullptr;
            }
            return ds::polymorphic_pointer_cast<T>(m_block->data);
        }

        // Runs `cb` on the main thread (next task_system::drain_main) once the resource is ready; never for a
        // failed one, and dropped if `owner` is destroyed first. Pending callbacks are bound to the handle's
        // task_system, so a handle that was not obtained from resource_system::require cannot use it.
        // `owner` holds `cb`: its captures go when the owner does, even while the block is still loading (#406).
        void then(const ds::lifetime& owner, std::function<void(T&)> cb) const {
            if (!m_block) return;
            ASSERT_MSG(m_tasks, "res_handle::then: the handle has no task_system");
            if (!m_tasks) return;
            m_block->on_settled([tasks = m_tasks, weak = owner.token(), held = ds::detail::held_by(owner, std::move(cb))](auto& block) {
                if (!block.data) {
                    ds::detail::release_callback(weak, held.lock().get());
                    return;
                }
                tasks->post_main([weak, held, data = block.data]() {
                    if (const auto callback = held.lock()) {
                        (*callback)(*ds::polymorphic_pointer_cast<T>(data));
                        ds::detail::release_callback(weak, callback.get());
                    }
                });
            });
        }

        const ::res::tag& get_tag() const
        {
            return m_tag;
        }

        T& operator*() const {
			auto ptr = get_sync();
			ASSERT_MSG(ptr, "Dereferencing a failed resource handle");
			return *ptr;
        }

		std::shared_ptr<T> operator->() const {
			auto ptr = get_sync();
			ASSERT_MSG(ptr, "Dereferencing a failed resource handle");
			return ptr;
		}

        std::shared_ptr<resource_control_block> get_control_block() const {
            return m_block;
		}

    private:
        std::shared_ptr<resource_control_block> m_block;
        ::res::tag m_tag;
        ds::task_system* m_tasks = nullptr;
    };

}
