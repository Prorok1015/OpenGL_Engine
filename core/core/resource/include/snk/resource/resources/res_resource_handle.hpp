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
        // On a task_system worker (an adapter that needs another resource) it runs queued compute tasks while it
        // waits, so the load it waits for cannot be stuck behind the workers that are all waiting. A load that is
        // already running on another thread is just waited for. Cycles through several threads are not detected
        // here (that is the wait graph of #134b); a wait that goes on for long is logged.
        // A handle built by hand, without a task_system, can only block on the block.
        void wait() const {
            if (!m_block) return;
            if (m_block->is_ready() || m_block->has_error()) return;
            if (m_tasks && m_tasks->mode() == ds::policy::threaded && ds::task_system::on_worker()) {
                help_until_finished();
                return;
            }
            if (m_tasks && m_tasks->mode() == ds::policy::manual) {
                // Nothing runs in the background, so run the queued loads here. When none is queued the block
                // may still be waiting for a resolver thread, whose completion queues the next one.
                while (!m_block->is_ready() && !m_block->has_error()) {
                    if (!m_tasks->run_one(ds::lane::compute) && !m_tasks->run_one(ds::lane::io)) {
                        m_block->wait_for(std::chrono::milliseconds(1));
                    }
                }
                return;
            }
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
        void then(const ds::lifetime& owner, std::function<void(T&)> cb) const {
            if (!m_block) return;
            ASSERT_MSG(m_tasks, "res_handle::then: the handle has no task_system");
            if (!m_tasks) return;
            m_block->on_settled([tasks = m_tasks, weak = owner.token(), cb = std::move(cb)](auto& block) {
                if (!block.data) return;
                tasks->post_main([weak, cb, data = block.data]() {
                    if (!weak.expired()) {
                        cb(*ds::polymorphic_pointer_cast<T>(data));
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
        void help_until_finished() const {
            const auto start = std::chrono::steady_clock::now();
            bool reported = false;
            while (!m_block->is_ready() && !m_block->has_error()) {
                if (m_tasks->try_run_one(ds::lane::compute)) continue;
                m_block->wait_for(std::chrono::milliseconds(1));
                if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                    reported = true;
                    egLOG_ERROR("resource", "still waiting for '{}' on a worker thread after 10 s: dependency cycle?", m_tag.view());
                }
            }
        }

        std::shared_ptr<resource_control_block> m_block;
        ::res::tag m_tag;
        ds::task_system* m_tasks = nullptr;
    };

}
