#pragma once
#include <filesystem>
#include <chrono>
#include <unordered_map>
#include <boost/asio.hpp>

namespace fs = std::filesystem;
namespace res {
    class file_watcher {
    public:
        file_watcher(boost::asio::io_context& io, std::function<void(fs::path)> notify_cb)
            : timer(io), m_notify_cb(notify_cb)
        {
        }

        void add_path(fs::path path) {
            watch_paths.push_back(std::move(path));
        }

        void start() {
            for (const auto& root : watch_paths) {
                update_state(root, false);
            }
            schedule_timer();
        }

        void stop() {
            timer.cancel();
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
                    if (notify) m_notify_cb(path);
                }
                else if (it->second != last_time) {
                    it->second = last_time;
                    if (notify) m_notify_cb(path);
                }
            }

            std::erase_if(file_states, [](const auto& item) {
                return !fs::exists(item.first);
                });
        }

        std::function<void(fs::path)> m_notify_cb;
        boost::asio::steady_timer timer;
        std::vector<fs::path> watch_paths;
        std::unordered_map<fs::path, fs::file_time_type> file_states;
    };
}