#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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

        bool has_error() const
        {
            return status == res_status::error;
		}

        bool is_ready() const
        {
            return status == res_status::ready;
        }

        void wait()
        {
            while (!is_ready() && !has_error())
            {
                std::this_thread::yield();
            }

            ASSERT_MSG(!has_error(), "Resource loading error: {0}", error_msg);
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
}