#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include "engine_assert.h"
#include "resources/res_resource_base.h"

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

        void wait()
        {
            while (!is_ready() && !has_error())
            {
                std::this_thread::yield();
            }

            ASSERT_MSG(!has_error(), "Resource loading error");
        }

        T& get() {
            wait();
            return data;
        }

        void then(std::function<void(res_control_block<T>&)> callback)
        {
            if (is_ready() || has_error()) {
                callback(*this);
            } else {
                std::lock_guard lock(callback_mtx);
                if (is_ready() || has_error()) {
                    callback(*this);
                    return;
				}
                on_ready_callbacks.push_back(callback);
            }
        }

        void set_ready(T&& result) {
            data = std::move(result);
            status = res_status::ready;

            std::lock_guard lock(callback_mtx);
            for (auto& cb : on_ready_callbacks) cb(*this);
            on_ready_callbacks.clear();
        }

        void set_error(std::string msg) {
            error_msg = std::move(msg);
            status = res_status::error;

            std::lock_guard lock(callback_mtx);
            for (auto& cb : on_ready_callbacks) cb(*this);
            on_ready_callbacks.clear();
        }
    };

    // Unified non-typed resource control block: all resource types share this block type,
    // data is retrieved via polymorphic_pointer_cast<T> on the stored shared_ptr<resource_entry>.
    using res_resource_block = res_control_block<std::shared_ptr<::res::resource_entry>>;
}
