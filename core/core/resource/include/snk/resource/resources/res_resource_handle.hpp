#pragma once
#include <snk/resource/resolvers/res_control_block.hpp>
#include <snk/resource/res_tag.h>
#include <snk/common/ds/ds_polymorphic_cast.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <chrono>
#include <concepts>
#include <format>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace core::res
{
    namespace detail {
        // res_handle::then with a functor of the caller's binary (#406): the block's callback and the main-queue item
        // are the engine's, the functor stays in `slot` (its owner empties it when it goes). The slot is called with a
        // pointer to the block's data, on the main thread; a failed block empties it without a call.
        SNK_API void then_on_main(const std::shared_ptr<res_resource_block>& block, ds::task_system& tasks,
            std::shared_ptr<ds::detail::held_slot> slot);
    }

    // Whether a loaded resource is a T: what a res_handle<T> accepts, also for the collector's field check (#615).
    template<typename T>
    bool holds(const ::res::resource_entry& entry)
    {
        return dynamic_cast<const T*>(&entry) != nullptr;
    }

    // The name a type mismatch gives for T: its desc __type when it has one.
    template<typename T>
    std::string_view handle_type_name()
    {
        if constexpr (requires { { T::__type } -> std::convertible_to<std::string_view>; }) {
            return T::__type;
        } else {
            return typeid(T).name();
        }
    }

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

        // A block cached for another request type can hold data that is not a T (a material file required as a
        // level, #615): such a handle is failed, while the block stays ready for the requests it fits.
        bool is_ready() const { return m_block && m_block->is_ready() && holds_t(); }
        bool has_error() const { return m_block && (m_block->has_error() || (m_block->is_ready() && !holds_t())); }
		bool is_valid() const { return m_block != nullptr; }

        std::shared_ptr<T> get() const {
            if (!m_block || !m_block->is_ready()) return nullptr;
            return std::dynamic_pointer_cast<T>(m_block->data);
        }

        // Why has_error(); empty when it has none.
        std::string error_text() const {
            if (!m_block) return {};
            if (m_block->has_error()) return m_block->error_msg;
            if (m_block->is_ready() && !holds_t()) return type_mismatch_text();
            return {};
        }

        // What res::diagnostics_collector::collect gives for the block, and the type mismatch of this request.
        void collect_into(::res::diagnostics_collector& collector) const {
            if (!m_block) return;
            collector.collect(m_block);
            if (m_block->is_ready() && !holds_t()) {
                collector.add(::res::diagnostic{ ::res::diagnostic_severity::error, std::string{ ::res::diagnostic_kind::type_mismatch },
                    std::string{ m_tag.view() }, "/__type", type_mismatch_text() });
            }
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
            auto typed = std::dynamic_pointer_cast<T>(m_block->data);
            if (!typed) {
                egLOG_ERROR("resource", "get_sync on failed resource '{}': {}", m_tag.view(), type_mismatch_text());
            }
            return typed;
        }

        // Runs `cb` on the main thread (next task_system::drain_main) once the resource is ready; never for a
        // failed one, and dropped if `owner` is destroyed first. Pending callbacks are bound to the handle's
        // task_system, so a handle that was not obtained from resource_system::require cannot use it.
        // `owner` holds `cb`: its captures go when the owner does, even while the block is still loading (#406).
        void then(const ds::lifetime& owner, std::function<void(T&)> cb) const {
            if (!m_block) return;
            ASSERT_MSG(m_tasks, "res_handle::then: the handle has no task_system");
            if (!m_tasks) return;
            using data_t = std::remove_cvref_t<decltype(m_block->data)>;
            detail::then_on_main(m_block, *m_tasks, ds::detail::hold(owner, [cb = std::move(cb), tag = m_tag](void* data) {
                const auto& entry = *static_cast<data_t*>(data);
                if (auto typed = std::dynamic_pointer_cast<T>(entry)) {
                    cb(*typed);
                    return;
                }
                egLOG_WARN("resource", "then on '{}' not called: {}", tag.view(), type_mismatch_text(tag, entry.get()));
            }));
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
        // Read only once the block is ready (its data is published then).
        bool holds_t() const {
            return !m_block->data || holds<T>(*m_block->data);
        }

        std::string type_mismatch_text() const {
            return type_mismatch_text(m_tag, m_block ? m_block->data.get() : nullptr);
        }

        static std::string type_mismatch_text(const ::res::tag& tag, const ::res::resource_entry* entry) {
            const std::string_view actual = entry && !entry->kind_name().empty() ? entry->kind_name() : std::string_view{ "another type" };
            return std::format("'{}' is a '{}', expected '{}'", tag.view(), actual, handle_type_name<T>());
        }

        std::shared_ptr<resource_control_block> m_block;
        ::res::tag m_tag;
        ds::task_system* m_tasks = nullptr;
    };

}
