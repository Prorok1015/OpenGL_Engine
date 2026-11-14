#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/asio.hpp>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
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

    namespace detail {
    struct watch_file_state {
        fs::file_time_type modified{};
        std::uintmax_t size = 0;
        bool operator==(const watch_file_state&) const = default;
    };

    struct watch_snapshot {
        std::map<fs::path, watch_file_state> files;
        std::set<fs::path> dirs;
    };

    // A file that vanished while another with the same size and modification time appeared is a rename, but only
    // when exactly one of each has that key: two candidates are reported as they are, never matched by a guess.
    inline std::vector<raw_change> diff_snapshots(const watch_snapshot& before, const watch_snapshot& after) {
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

        // Paths by (size, modification time): a key held by exactly one vanished and one new file is a rename.
        using key_type = std::pair<std::uintmax_t, std::int64_t>;
        const auto key_of = [](const watch_file_state& state) {
            return key_type{ state.size, static_cast<std::int64_t>(state.modified.time_since_epoch().count()) };
        };
        std::map<key_type, std::vector<fs::path>> gone_by_key;
        std::map<key_type, std::vector<fs::path>> new_by_key;
        for (const auto& path : gone_files) gone_by_key[key_of(before.files.at(path))].push_back(path);
        for (const auto& path : new_files) new_by_key[key_of(after.files.at(path))].push_back(path);
        std::vector<raw_change> renamed;
        std::vector<raw_change> deleted;
        std::vector<raw_change> created;
        std::set<fs::path> paired_new;
        std::set<fs::path> paired_gone;
        for (const auto& [key, old_paths] : gone_by_key) {
            const auto candidates = new_by_key.find(key);
            if (old_paths.size() != 1 || candidates == new_by_key.end() || candidates->second.size() != 1) continue;
            renamed.push_back({ candidates->second.front(), old_paths.front(), change_kind::renamed, false });
            paired_new.insert(candidates->second.front());
            paired_gone.insert(old_paths.front());
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

    // Compared by path elements: "root/sub" is not above "root/subway", and a root may end with a separator.
    inline bool is_at_or_below(const fs::path& path, const fs::path& top) {
        if (top.empty()) return false;
        const auto mismatch = std::mismatch(top.begin(), top.end(), path.begin(), path.end());
        if (mismatch.first == top.end()) return true;
        return mismatch.first->empty() && std::next(mismatch.first) == top.end();
    }

    // What a scan could not read keeps what was known at and below it: a listing cut short would report the files
    // it did not reach as deleted, and as created again by the next poll.
    inline void keep_unread(const watch_snapshot& before, const std::vector<fs::path>& unread, watch_snapshot& after) {
        for (const auto& top : unread) {
            std::erase_if(after.files, [&top](const auto& item) { return is_at_or_below(item.first, top); });
            std::erase_if(after.dirs, [&top](const fs::path& dir) { return is_at_or_below(dir, top); });
            for (const auto& [path, state] : before.files) {
                if (is_at_or_below(path, top)) after.files.insert_or_assign(path, state);
            }
            for (const auto& dir : before.dirs) {
                if (is_at_or_below(dir, top)) after.dirs.insert(dir);
            }
        }
    }
    }

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
                m_snapshot = scan_all().found;
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
                const auto state = state_of(path, ec);
                if (ec) return;
                std::lock_guard lock(m_state_mutex);
                m_snapshot.files[path] = state;
                for (fs::path parent = path.parent_path(); is_below_a_root(parent); parent = parent.parent_path()) {
                    m_snapshot.dirs.insert(parent);
                }
                return;
            }
            std::error_code ec;
            const auto written = fs::last_write_time(path, ec);
            if (!ec) {
                std::lock_guard lock(m_state_mutex);
                file_states[path] = written;
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
            const auto state = state_of(to, ec);
            std::lock_guard lock(m_state_mutex);
            m_snapshot.files.erase(from);
            if (ec) return;
            m_snapshot.files[to] = state;
            for (fs::path parent = to.parent_path(); is_below_a_root(parent); parent = parent.parent_path()) {
                m_snapshot.dirs.insert(parent);
            }
        }

    private:
        static detail::watch_file_state state_of(const fs::path& path, std::error_code& ec) {
            detail::watch_file_state state;
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

                // Whatever one poll throws (the file system, a callback of whoever watches), the next one is scheduled:
                // an exception that left this handler used to end the watching for good, without a word (#939).
                m_poll_failed = false;
                try {
                    if (m_batch_mode) {
                        poll_changes();
                    }
                    else {
                        for (const auto& root : watch_paths) {
                            update_state(root, true);
                        }
                    }
                    if (!m_poll_failed) m_last_poll_error.clear();
                }
                catch (const std::exception& e) {
                    report_poll_error(e.what());
                }
                catch (...) {
                    report_poll_error("unknown exception");
                }
                // A callback of this very poll may have called stop(): the timer is not armed again after it.
                if (!m_stopped) schedule_timer();
            });
        }

        struct scan_result {
            detail::watch_snapshot found;
            // Roots, folders and files the scan could not read (detail::keep_unread).
            std::vector<fs::path> unread;
        };

        // Every accepted regular file and every folder below the roots, without entering ignored folders and without
        // following links: a link is neither a file nor a folder here.
        scan_result scan_all() {
            scan_result result;
            for (const auto& root : watch_paths) {
                std::error_code ec;
                const auto root_status = fs::status(root, ec);
                if (!fs::is_directory(root_status)) {
                    // A root that is gone or is no folder has nothing to list; one that cannot be asked about is unread.
                    if (!fs::status_known(root_status)) result.unread.push_back(root);
                    continue;
                }
                ec.clear();
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                const fs::recursive_directory_iterator end;
                for (; !ec && it != end; it.increment(ec)) {
                    std::error_code entry_ec;
                    const auto status = it->symlink_status(entry_ec);
                    if (entry_ec) {
                        result.unread.push_back(it->path());
                        it.disable_recursion_pending();
                        continue;
                    }
                    const std::string name = fs_paths::utf8_of(it->path().filename());
                    if (fs::is_directory(status)) {
                        if (m_options.ignore_dir && m_options.ignore_dir(name)) {
                            it.disable_recursion_pending();
                            continue;
                        }
                        result.found.dirs.insert(it->path());
                    }
                    else if (fs::is_regular_file(status) && (!m_options.accept_file || m_options.accept_file(name))) {
                        const auto state = state_of(it->path(), entry_ec);
                        if (entry_ec) result.unread.push_back(it->path());
                        else result.found.files.emplace(it->path(), state);
                    }
                }
                if (ec) {
                    result.unread.push_back(root);
                    report_poll_error(std::format("the scan of '{}' was cut short: {}",
                        fs_paths::utf8_of(root), ec.message()));
                }
            }
            return result;
        }

        void poll_changes() {
            scan_result scan = scan_all();
            std::vector<raw_change> batch;
            {
                std::lock_guard lock(m_state_mutex);
                detail::keep_unread(m_snapshot, scan.unread, scan.found);
                batch = detail::diff_snapshots(m_snapshot, scan.found);
                m_snapshot = std::move(scan.found);
            }
            for (const auto& change : batch) {
                if (m_notify_cb && !change.is_directory
                    && (change.kind == change_kind::created || change.kind == change_kind::modified)) {
                    deliver(m_notify_cb, change.path);
                }
            }
            if (m_batch_cb && !batch.empty()) {
                deliver(m_batch_cb, std::move(batch));
            }
        }

        // A callback that throws loses its own change only: the rest of the poll is still delivered.
        template <typename Callback, typename Arg>
        void deliver(const Callback& callback, Arg&& arg) {
            try {
                callback(std::forward<Arg>(arg));
            }
            catch (const std::exception& e) {
                egLOG_ERROR("resource", "a callback of the file watcher threw: {}", e.what());
            }
            catch (...) {
                egLOG_ERROR("resource", "a callback of the file watcher threw an unknown exception");
            }
        }

        // Once per distinct failure, not twice a second for as long as it lasts.
        void report_poll_error(std::string_view what) {
            m_poll_failed = true;
            if (m_last_poll_error == what) return;
            m_last_poll_error = what;
            egLOG_WARN("resource", "a poll of the watched files failed and is tried again: {}", what);
        }

        // Files come and go while they are listed (another thread renames a temporary file into place): nothing here
        // throws for that. A file that cannot be looked at is left for the next poll.
        void update_state(const fs::path& root, bool notify) {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) return;

            std::vector<fs::path> changed;

            {
                std::lock_guard lock(m_state_mutex);
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                const fs::recursive_directory_iterator end;
                for (; !ec && it != end; it.increment(ec)) {
                    std::error_code entry_ec;
                    if (!it->is_regular_file(entry_ec) || entry_ec) continue;

                    const fs::path path = it->path();
                    const auto last_time = fs::last_write_time(path, entry_ec);
                    if (entry_ec) continue;

                    auto known = file_states.find(path);
                    if (known == file_states.end()) {
                        file_states[path] = last_time;
                        if (notify) changed.push_back(path);
                    }
                    else if (known->second != last_time) {
                        known->second = last_time;
                        if (notify) changed.push_back(path);
                    }
                }

                // Only what is known to be gone: a file that could not be asked about stays known.
                std::erase_if(file_states, [](const auto& item) {
                    std::error_code gone;
                    return !fs::exists(item.first, gone) && !gone;
                });
            }

            for (const auto& path : changed) {
                deliver(m_notify_cb, path);
            }
        }

        std::function<void(fs::path)> m_notify_cb;
        boost::asio::steady_timer timer;
        std::atomic<bool> m_stopped{ false };
        std::string m_last_poll_error;
        bool m_poll_failed = false;
        std::vector<fs::path> watch_paths;
        std::mutex m_state_mutex;
        std::unordered_map<fs::path, fs::file_time_type> file_states;
        watch_options m_options;
        std::function<void(std::vector<raw_change>)> m_batch_cb;
        bool m_batch_mode = false;
        detail::watch_snapshot m_snapshot;
    };
}
