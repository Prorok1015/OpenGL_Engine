#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/asio.hpp>
#include <snk/resource/res_file_change.h>

namespace fs = std::filesystem;
namespace res {
    // What one poll of a watched mount found, before it becomes tags (file_change).
    struct raw_change {
        fs::path path;
        // Set for change_kind::renamed.
        fs::path old_path;
        change_kind kind = change_kind::modified;
        bool is_directory = false;
    };

    class file_watcher {
    public:
        file_watcher(boost::asio::io_context& io)
            : timer(io)
        {}

        file_watcher(boost::asio::io_context& io, std::function<void(fs::path)> notify_cb)
            : m_notify_cb(notify_cb), timer(io)
        {
        }

        void add_path(fs::path path) {
            watch_paths.push_back(std::move(path));
        }

        // Before start(). With options.report_changes the watcher also reports created, deleted and renamed files and
        // folders, a batch per poll, to `batch_cb` (on the io thread); the files whose resource changed still go to the
        // per-file callback. Folders named by options.ignore_dir are not entered.
        void report_changes(watch_options options, std::function<void(std::vector<raw_change>)> batch_cb) {
            m_options = std::move(options);
            m_batch_cb = std::move(batch_cb);
            m_batch_mode = m_options.report_changes && m_batch_cb;
        }

        bool reports_changes() const { return m_batch_mode; }

        void start() {
            if (m_batch_mode) {
                std::lock_guard lock(m_state_mutex);
                m_snapshot = scan_all();
            }
            else {
                for (const auto& root : watch_paths) {
                    update_state(root, false);
                }
            }
            schedule_timer();
        }

        // On the io thread: a poll that already fired is queued with success and would re-arm the timer after a
        // cancel alone, so io_context::run() would never run out of work.
        void stop() {
            m_stopped = true;
            timer.cancel();
        }

        // Update the snapshot for a file we just wrote ourselves,
        // so the next poll does not treat it as an external change.
        void mark_written(const fs::path& path) {
            if (m_batch_mode) {
                std::error_code ec;
                const auto state = file_state_of(path, ec);
                if (ec) return;
                std::lock_guard lock(m_state_mutex);
                m_snapshot.files[path] = state;
                for (fs::path parent = path.parent_path(); is_below_a_root(parent); parent = parent.parent_path()) {
                    m_snapshot.dirs.insert(parent);
                }
                return;
            }
            if (fs::exists(path)) {
                std::lock_guard lock(m_state_mutex);
                file_states[path] = fs::last_write_time(path);
            }
        }

        // The file was removed or moved by us (batch mode): the next poll does not report it.
        void mark_removed(const fs::path& path) {
            if (!m_batch_mode) return;
            std::lock_guard lock(m_state_mutex);
            m_snapshot.files.erase(path);
        }

        void mark_renamed(const fs::path& from, const fs::path& to) {
            if (!m_batch_mode) return;
            std::error_code ec;
            const auto state = file_state_of(to, ec);
            std::lock_guard lock(m_state_mutex);
            m_snapshot.files.erase(from);
            if (ec) return;
            m_snapshot.files[to] = state;
            for (fs::path parent = to.parent_path(); is_below_a_root(parent); parent = parent.parent_path()) {
                m_snapshot.dirs.insert(parent);
            }
        }

    private:
        struct file_state {
            fs::file_time_type modified{};
            std::uintmax_t size = 0;
            bool operator==(const file_state&) const = default;
        };

        struct snapshot {
            std::map<fs::path, file_state> files;
            std::set<fs::path> dirs;
        };

        static file_state file_state_of(const fs::path& path, std::error_code& ec) {
            file_state state;
            state.modified = fs::last_write_time(path, ec);
            if (ec) return state;
            state.size = fs::file_size(path, ec);
            return state;
        }

        bool is_below_a_root(const fs::path& path) const {
            return std::ranges::any_of(watch_paths, [&path](const fs::path& root) {
                return path != root && path.native().size() > root.native().size()
                    && path.native().compare(0, root.native().size(), root.native()) == 0;
            });
        }

        void schedule_timer() {
            timer.expires_after(std::chrono::milliseconds(500));
            timer.async_wait([this](auto ec) {
                if (ec || m_stopped) return;

                if (m_batch_mode) {
                    poll_changes();
                }
                else {
                    for (const auto& root : watch_paths) {
                        update_state(root, true);
                    }
                }
                schedule_timer();
            });
        }

        // Every accepted regular file and every folder below the roots, without entering ignored folders and without
        // following links: a link is neither a file nor a folder here.
        snapshot scan_all() const {
            snapshot found;
            for (const auto& root : watch_paths) {
                std::error_code ec;
                if (!fs::is_directory(root, ec)) continue;
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                const fs::recursive_directory_iterator end;
                for (; !ec && it != end; it.increment(ec)) {
                    std::error_code entry_ec;
                    const auto status = it->symlink_status(entry_ec);
                    if (entry_ec) continue;
                    const std::string name = it->path().filename().string();
                    if (fs::is_directory(status)) {
                        if (m_options.ignore_dir && m_options.ignore_dir(name)) {
                            it.disable_recursion_pending();
                            continue;
                        }
                        found.dirs.insert(it->path());
                    }
                    else if (fs::is_regular_file(status) && (!m_options.accept_file || m_options.accept_file(name))) {
                        const auto state = file_state_of(it->path(), entry_ec);
                        if (!entry_ec) found.files.emplace(it->path(), state);
                    }
                }
            }
            return found;
        }

        void poll_changes() {
            snapshot now = scan_all();
            std::vector<raw_change> batch;
            {
                std::lock_guard lock(m_state_mutex);
                batch = diff(m_snapshot, now);
                m_snapshot = std::move(now);
            }
            for (const auto& change : batch) {
                if (m_notify_cb && !change.is_directory
                    && (change.kind == change_kind::created || change.kind == change_kind::modified)) {
                    m_notify_cb(change.path);
                }
            }
            if (m_batch_cb && !batch.empty()) {
                m_batch_cb(std::move(batch));
            }
        }

        // A file that vanished while another with the same size and modification time appeared is a rename, but only
        // when exactly one of each has that key: two candidates are reported as they are, never matched by a guess.
        static std::vector<raw_change> diff(const snapshot& before, const snapshot& after) {
            std::vector<fs::path> gone_files;
            std::vector<fs::path> new_files;
            std::vector<raw_change> changed;
            for (const auto& [path, state] : before.files) {
                if (!after.files.contains(path)) gone_files.push_back(path);
            }
            for (const auto& [path, state] : after.files) {
                const auto old = before.files.find(path);
                if (old == before.files.end()) new_files.push_back(path);
                else if (!(old->second == state)) changed.push_back({ path, {}, change_kind::modified, false });
            }

            const auto key_count = [](const std::vector<fs::path>& paths, const std::map<fs::path, file_state>& files,
                                       const file_state& key) {
                return std::ranges::count_if(paths, [&](const fs::path& path) { return files.at(path) == key; });
            };
            std::vector<raw_change> renamed;
            std::vector<raw_change> deleted;
            std::vector<raw_change> created;
            std::set<fs::path> paired_new;
            std::set<fs::path> paired_gone;
            for (const auto& old_path : gone_files) {
                const file_state& key = before.files.at(old_path);
                if (key_count(gone_files, before.files, key) != 1 || key_count(new_files, after.files, key) != 1) continue;
                const auto target = std::ranges::find_if(new_files, [&](const fs::path& path) { return after.files.at(path) == key; });
                renamed.push_back({ *target, old_path, change_kind::renamed, false });
                paired_new.insert(*target);
                paired_gone.insert(old_path);
            }
            for (auto it = gone_files.rbegin(); it != gone_files.rend(); ++it) {
                if (!paired_gone.contains(*it)) deleted.push_back({ *it, {}, change_kind::deleted, false });
            }
            for (const auto& path : new_files) {
                if (!paired_new.contains(path)) created.push_back({ path, {}, change_kind::created, false });
            }

            std::vector<raw_change> batch = std::move(renamed);
            for (auto it = before.dirs.rbegin(); it != before.dirs.rend(); ++it) {
                if (!after.dirs.contains(*it)) batch.push_back({ *it, {}, change_kind::deleted, true });
            }
            std::ranges::move(deleted, std::back_inserter(batch));
            for (const auto& path : after.dirs) {
                if (!before.dirs.contains(path)) batch.push_back({ path, {}, change_kind::created, true });
            }
            std::ranges::move(created, std::back_inserter(batch));
            std::ranges::move(changed, std::back_inserter(batch));
            return batch;
        }

        void update_state(const fs::path& root, bool notify) {
            if (!fs::exists(root)) return;

            std::vector<fs::path> changed;

            {
                std::lock_guard lock(m_state_mutex);
                for (const auto& entry : fs::recursive_directory_iterator(root)) {
                    if (!entry.is_regular_file()) continue;

                    auto path = entry.path();
                    auto last_time = fs::last_write_time(path);

                    auto it = file_states.find(path);
                    if (it == file_states.end()) {
                        file_states[path] = last_time;
                        if (notify) changed.push_back(path);
                    }
                    else if (it->second != last_time) {
                        it->second = last_time;
                        if (notify) changed.push_back(path);
                    }
                }

                std::erase_if(file_states, [](const auto& item) {
                    return !fs::exists(item.first);
                });
            }

            for (const auto& path : changed) {
                m_notify_cb(path);
            }
        }

        std::function<void(fs::path)> m_notify_cb;
        boost::asio::steady_timer timer;
        std::atomic<bool> m_stopped{ false };
        std::vector<fs::path> watch_paths;
        std::mutex m_state_mutex;
        std::unordered_map<fs::path, fs::file_time_type> file_states;
        watch_options m_options;
        std::function<void(std::vector<raw_change>)> m_batch_cb;
        bool m_batch_mode = false;
        snapshot m_snapshot;
    };
}
