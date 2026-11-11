#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <vector>

// Watches the repository of the project for changes made outside the editor (#485): a commit, checkout or pull in a
// terminal, an edit of a top-level file. Polls file stamps (write time, size, existence) at most every k_poll_interval
// from tick() on the main thread; git is never run. Main thread only.
namespace edt
{
	class vcs_service;
}

namespace edt::vcs
{
	struct file_stamp
	{
		// Write time in the clock's own ticks: only compared.
		std::int64_t mtime = 0;
		std::uint64_t size = 0;

		bool operator==(const file_stamp&) const = default;
	};

	struct dir_entry
	{
		std::filesystem::path name{};
		bool is_dir = false;
	};

	struct repository_changes
	{
		// The index or a top-level file of the work tree: the status may differ.
		bool index = false;
		// HEAD, refs/, packed-refs, FETCH_HEAD, ORIG_HEAD, MERGE_HEAD: the status and the history may differ.
		bool refs = false;

		bool any() const { return index || refs; }
	};

	class repository_watcher
	{
	public:
		using clock = std::chrono::steady_clock;

		// std::filesystem with error codes: never throws.
		static std::optional<file_stamp> default_stat(const std::filesystem::path& path);
		static std::vector<dir_entry> default_list_dir(const std::filesystem::path& path);

		struct ports
		{
			std::function<clock::time_point()> now = [] { return clock::now(); };
			// nullopt: no such file or folder (or it cannot be read). A folder's size is 0.
			std::function<std::optional<file_stamp>(const std::filesystem::path&)> stat_file = default_stat;
			// The entries of a folder; empty when it cannot be read.
			std::function<std::vector<dir_entry>(const std::filesystem::path&)> list_dir = default_list_dir;
			// Whether a git write of the editor (vcs_service::running_writes) runs now; null: never.
			std::function<bool()> own_writes_running{};
		};

		static constexpr std::chrono::milliseconds k_poll_interval{ 500 };
		// Past this many entries under refs/ (at once), or this long walking them k_slow_walks_to_switch times in a
		// row, only the folders under refs/ are watched.
		static constexpr std::size_t k_refs_budget = 4096;
		static constexpr std::chrono::microseconds k_refs_walk_budget{ 2000 };
		static constexpr int k_slow_walks_to_switch = 3;

		explicit repository_watcher(ports ports);

		// Re-arms on the repository of a new project: everything is taken as it is now, nothing is reported. An empty
		// `git_dir` turns the watcher off. `common_dir` empty: `git_dir`; `project_root` may equal `repo_root`.
		void set_paths(const std::filesystem::path& git_dir, const std::filesystem::path& common_dir,
			const std::filesystem::path& repo_root, const std::filesystem::path& project_root);
		bool armed() const { return m_armed; }
		// Not armed and the project is no repository: each poll looks only for its marker (vcs::repository_marker, a
		// stat per folder) and reports `refs` once when one appears or goes, so the Git panel asks vcs.info, which probes
		// again (#561). An empty `project_root` stops it; set_paths does too.
		void watch_marker(const std::filesystem::path& project_root);
		bool watching_marker() const { return m_marker_root.has_value(); }
		const std::filesystem::path& watched_git_dir() const { return m_git_dir; }

		// Re-arms from the service's cached state when its state_generation() moved since the last call (never probes:
		// the watcher follows whatever probed, e.g. vcs.info or the Git panel's refresh). Off while the open project is
		// not the one the state was probed for (a switch nothing has probed yet).
		void follow(const vcs_service& service);

		// A git write of the editor starts (vcs_service's write observer), before its git runs: an index change seen up
		// to now is someone else's and is reported by the next tick; until the editor's last write has ended (seen in
		// tick) the index and the work tree are not compared, and then taken as they are. Refs are always compared.
		void begin_own_write();

		// What changed since the last poll; nothing until k_poll_interval passed since it.
		repository_changes tick();

		// Only the folders under refs/ are watched (too many refs, or walking them was too slow).
		bool refs_folders_only() const { return m_refs_folders_only; }

	private:
		using stamps = std::map<std::filesystem::path, file_stamp>;

		enum class walk_result : std::uint8_t { done, too_many, too_slow };

		stamps index_stamps() const;
		// HEAD, FETCH_HEAD, ORIG_HEAD, MERGE_HEAD, packed-refs.
		stamps fixed_refs_stamps() const;
		// refs/: every file, or (folders only) the folders found by the last folder walk; nullopt when a slow walk was
		// given up (nothing known this time). `switched`: this call moved to folders only (nothing to compare with:
		// tick reports refs once rather than lose a change).
		std::optional<stamps> walked_refs_stamps(bool& switched);
		// Every file under refs/, given up past the entry or time budget.
		walk_result walk_ref_files(stamps& out) const;
		// Every folder under refs/ (refs/ included), at most k_refs_budget.
		std::vector<std::filesystem::path> walk_ref_folders() const;
		void add(stamps& out, const std::filesystem::path& path) const;
		std::optional<std::filesystem::path> marker_now() const;
		repository_changes tick_marker();

		ports m_ports;
		bool m_armed = false;
		std::filesystem::path m_git_dir;
		std::filesystem::path m_common_dir;
		std::vector<std::filesystem::path> m_work_roots;
		// set_paths' arguments.
		std::vector<std::filesystem::path> m_armed_with;

		stamps m_index;
		stamps m_fixed_refs;
		// nullopt: no walk finished since arming.
		std::optional<stamps> m_walked_refs;
		int m_slow_walks = 0;
		bool m_refs_folders_only = false;
		bool m_warned = false;
		std::vector<std::filesystem::path> m_ref_folders;

		std::optional<clock::time_point> m_last_poll;
		bool m_own_write = false;
		// An index change found when an own write began, reported by the next tick.
		bool m_pending_index = false;
		std::optional<std::uint64_t> m_followed_generation;
		std::optional<std::filesystem::path> m_marker_root;
		std::optional<std::filesystem::path> m_marker;
	};
}
