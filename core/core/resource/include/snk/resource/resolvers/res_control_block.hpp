#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <memory>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/resources/res_resource_base.h>

namespace core::res
{
    enum class res_status : uint8_t {
		pending,    // push to load queue
		loading,    // loading from source
		processing, // processing by adapter
		ready,      // ready to use
        error       // woops
    };

    struct res_control_block_base {
        std::string error_msg;
        std::atomic<res_status> status = res_status::pending;
        mutable std::mutex callback_mtx;

        virtual ~res_control_block_base() = default;
    };

    template<typename T>
    struct res_control_block : res_control_block_base {
        T data;
        std::vector<std::function<void(res_control_block<T>&)>> on_ready_callbacks;

        bool is_ready() const {
            return status == res_status::ready;
        }

        bool has_error() const {
            return status == res_status::error;
		}

        // Blocks until the block is ready or failed. Does not assert on error: check has_error().
        void wait()
        {
            if (is_finished()) return;
            std::unique_lock lock(callback_mtx);
            m_cv.wait(lock, [this] { return is_finished(); });
        }

        T& get() {
            wait();
            return data;
        }

        // Resource-pipeline hook, not for gameplay code (res_handle::then delivers on the main thread).
        // Fires exactly once, on whichever thread settles the block, or immediately (on the caller
        // thread) if it already has. Callbacks never run under the lock, so they may re-enter this block.
        void on_settled(std::function<void(res_control_block<T>&)> callback)
        {
            {
                std::lock_guard lock(callback_mtx);
                if (!is_finished()) {
                    on_ready_callbacks.push_back(std::move(callback));
                    return;
                }
            }
            invoke_callback(callback);
        }

        void set_ready(T&& result) {
            // data is published before the release store of status, under the same lock that guards on_settled()/wait()
            finish(res_status::ready, [&] { data = std::move(result); });
        }

        void set_error(std::string msg) {
            finish(res_status::error, [&] { error_msg = std::move(msg); });
        }

    private:
        bool is_finished() const {
            auto s = status.load(std::memory_order_acquire);
            return s == res_status::ready || s == res_status::error;
        }

        template<typename F>
        void finish(res_status final_status, F&& write)
        {
            std::vector<std::function<void(res_control_block<T>&)>> callbacks;
            {
                std::lock_guard lock(callback_mtx);
                // A block settles once: a second set_ready/set_error must not overwrite data or drop callbacks.
                if (is_finished()) {
                    egLOG_ERROR("resource", "res_control_block already finished, ignoring repeated completion");
                    return;
                }
                write();
                status.store(final_status, std::memory_order_release);
                callbacks.swap(on_ready_callbacks);
            }
            m_cv.notify_all();
            for (auto& cb : callbacks) invoke_callback(cb);
        }

        // A throwing callback must not prevent the remaining callbacks from firing nor turn a ready block into an error.
        void invoke_callback(std::function<void(res_control_block<T>&)>& cb)
        {
            try {
                cb(*this);
            } catch (const std::exception& e) {
                egLOG_ERROR("resource", "on_settled() callback threw: {}", e.what());
            } catch (...) {
                egLOG_ERROR("resource", "on_settled() callback threw an unknown exception");
            }
        }

        std::condition_variable m_cv;
    };

    // Unified non-typed resource control block: all resource types share this block type,
    // data is retrieved via polymorphic_pointer_cast<T> on the stored shared_ptr<resource_entry>.
    using res_resource_block = res_control_block<std::shared_ptr<::res::resource_entry>>;
}
