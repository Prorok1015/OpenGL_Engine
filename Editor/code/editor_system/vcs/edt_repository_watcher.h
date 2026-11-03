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
			// Whether a git job of the editor (vcs_service::running_jobs) runs now; null: never.
			std::function<bool()> own_jobs_running{};
		};

		static constexpr std::chrono::milliseconds k_poll_interval{ 500 };
		// Past this many entries under refs/, or this long walking them, only the folders under refs/ are watched.
		static constexpr std::size_t k_refs_budget = 4096;
		static constexpr std::chrono::microseconds k_refs_walk_budget{ 2000 };

		explicit repository_watcher(ports ports);

		// Re-arms on the repository of a new project: everything is taken as it is now, nothing is reported. An empty
		// `git_dir` turns the watcher off. `common_dir` empty: `git_dir`; `project_root` may equal `repo_root`.
		void set_paths(const std::filesystem::path& git_dir, const std::filesystem::path& common_dir,
			const std::filesystem::path& repo_root, const std::filesystem::path& project_root);
		bool armed() const { return m_armed; }

		// A git job of the editor starts (vcs_service's job observer), before its git runs: an index change seen up to
		// now is someone else's and is reported by the next tick; until the editor's last job has ended (seen in tick)
		// the index and the work tree are not compared, and then taken as they are. Refs are always compared.
		void begin_own_job();

		// What changed since the last poll; nothing until k_poll_interval passed since it.
		repository_changes tick();

		// Only the folders under refs/ are watched (too many refs, or walking them was too slow).
		bool refs_folders_only() const { return m_refs_folders_only; }

	private:
		using stamps = std::map<std::filesystem::path, file_stamp>;

		stamps index_stamps() const;
		// The fixed refs files and refs/: every file, or (folders only) the folders found by the last walk.
		stamps refs_stamps();
		// Every file under refs/; false past the entry or time budget.
		bool walk_ref_files(stamps& out) const;
		// Every folder under refs/ (refs/ included), at most k_refs_budget.
		std::vector<std::filesystem::path> walk_ref_folders() const;
		void add(stamps& out, const std::filesystem::path& path) const;

		ports m_ports;
		bool m_armed = false;
		std::filesystem::path m_git_dir;
		std::filesystem::path m_common_dir;
		std::vector<std::filesystem::path> m_work_roots;

		stamps m_index;
		stamps m_refs;
		bool m_refs_folders_only = false;
		bool m_warned = false;
		std::vector<std::filesystem::path> m_ref_folders;

		std::optional<clock::time_point> m_last_poll;
		bool m_own_job = false;
		// An index change found when an own job began, reported by the next tick.
		bool m_pending_index = false;
	};
}
