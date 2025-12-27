#pragma once
#include "res_system.h"
#include <filesystem>
#include <chrono>
#include <unordered_map>
#include <boost/asio.hpp>

namespace fs = std::filesystem;
namespace res {
    class file_watcher {
    public:
        file_watcher(resource_system& res_sys)
            : timer(io), res_system(res_sys)
        {
        }

        ~file_watcher()
        {
            timer.cancel();
        }
        void add_path(fs::path path) {
            watch_paths.push_back(std::move(path));
        }

        void start() {
            for (const auto& root : watch_paths) {
                update_state(root, false);
            }
            schedule_timer();
            async_thread = std::async(std::launch::async, [this]() { io.run(); });
        }

    private:
        void schedule_timer() {
            timer.expires_after(std::chrono::milliseconds(500));
            timer.async_wait([this](auto ec) {
                if (ec) return;

                for (const auto& root : watch_paths) {
                    update_state(root, true);
                }
                schedule_timer();
            });
        }

        void update_state(const fs::path& root, bool notify) {
            if (!fs::exists(root)) return;

            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) continue;

                auto path = entry.path();
                auto last_time = fs::last_write_time(path);

                auto it = file_states.find(path);
                if (it == file_states.end()) {
                    file_states[path] = last_time;
                    if (notify) res_system.signal_changed(path);
                }
                else if (it->second != last_time) {
                    it->second = last_time;
                    if (notify) res_system.signal_changed(path);
                }
            }

            std::erase_if(file_states, [](const auto& item) {
                return !fs::exists(item.first);
                });
        }

        std::future<void> async_thread;
        boost::asio::io_context io;
        boost::asio::steady_timer timer;
        resource_system& res_system;
        std::vector<fs::path> watch_paths;
        std::unordered_map<fs::path, fs::file_time_type> file_states;
    };
}