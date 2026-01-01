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
    };

    template<typename T>
    struct res_control_block : res_control_block_base {
        T data;

        std::vector<std::function<void(res_control_block<T>&)>> on_ready_callbacks;

        T& get()
        {
            while (status != res_status::ready)
            {
				ASSERT_MSG(status != res_status::error, "Resource loading error: {0}", error_msg);
                std::this_thread::yield();
            }
            return data;
        }

        void then(std::function<void(res_control_block<T>&)> callback)
        {
            if (status == res_status::ready) {
                callback(*this);
            } else {
                std::lock_guard lock(callback_mtx);
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