#pragma once
#include "edt_commit_graph.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The History tab of the Git panel (#457), without ImGui: vcs.log pages loaded only as the rows come into view, laid
// out by commit_graph as they arrive; the details of one commit (vcs.show_files, then vcs.diff of a file). Every call
// is a command run through `invoke` as a person's call; its job's result comes back through
// editor_events::job_finished. Main thread only.
namespace edt::vcs
{
	struct history_ref
	{
		enum class kind : std::uint8_t { head, branch, remote, tag };
		std::string name;
		kind type = kind::branch;
	};

	struct history_commit
	{
		std::string hash;
		// First parent first.
		std::vector<std::string> parents;
		std::string author;
		std::string email;
		// Unix seconds.
		std::int64_t time = 0;
		std::string subject;
		std::vector<history_ref> refs;
	};

	struct history_file
	{
		std::string path;
		// A renamed or copied file: where it came from.
		std::string orig_path;
		// git's --name-status letter: A, M, D, R, C, T ...
		char status = 'M';
	};

	struct history_filter
	{
		// Empty: every local branch, remote-tracking branch and tag.
		std::string branch;
		std::string author;
		// Text in the commit message.
		std::string text;

		bool operator==(const history_filter&) const = default;
	};

	// One commit of a vcs.log page; nullopt when it has no hash. Unknown ref kinds are left out. Pure.
	std::optional<history_commit> history_commit_of(const boost::json::object& value);
	// The files of a vcs.show_files result. Pure.
	std::vector<history_file> history_files_of(const boost::json::object& result);

	class history_model
	{
	public:
		using clock = std::chrono::steady_clock;

		struct ports
		{
			// A command of the registry, as a person's call (origin ui).
			std::function<cmd::result(std::string_view id, boost::json::value args)> invoke;
			// job_service::cancel.
			std::function<bool(std::uint64_t job)> cancel;
			std::function<clock::time_point()> now = [] { return clock::now(); };
			// vcs.log takes all/author/grep and vcs.show_files and vcs.diff{commit} exist (#457 PR-1). Without them the
			// history is HEAD's (or the branch's), the author and text filters are not sent and a selection reads no
			// files.
			bool extended_reads = false;
		};

		static constexpr int k_page_size = 200;
		// The next page is asked for once the last row on screen is this close to the last row loaded.
		static constexpr std::size_t k_prefetch_rows = 50;
		// Typing in a filter field reloads after this quiet time.
		static constexpr std::chrono::milliseconds k_filter_delay{ 300 };
		// vcs.log answers `stale` when the snapshot behind the cursor is gone: the history starts again from the top,
		// but not forever while refs keep moving.
		static constexpr int k_max_restarts = 3;

		history_model(editor_events& events, ports ports);
		// Cancels the jobs it still waits for.
		~history_model();
		// The job_finished slot holds `this`.
		history_model(const history_model&) = delete;
		history_model& operator=(const history_model&) = delete;

		// The tab's frame (shown) or its hidden frames. Shown for the first time or again after being hidden, it loads
		// from the top.
		void set_visible(bool visible);
		// Runs a filter change once it is due.
		void tick();
		// From the top, with the filter as it is (Refresh, or after a commit, pull or checkout).
		void reload();
		// A new filter drops the selection (the commit may not pass it).
		void set_filter(history_filter filter);
		const history_filter& filter() const { return m_filter; }
		// The author and text filters are sent (ports::extended_reads).
		bool filters_by_text() const { return m_ports.extended_reads; }
		// Rows [first, last] are on screen: loads the next page when `last` is near the end of what is loaded.
		void show_rows(std::size_t first, std::size_t last);

		void select(std::size_t row);
		void select_file(const std::string& path);

		const std::vector<history_commit>& commits() const { return m_commits; }
		const commit_graph& graph() const { return m_graph; }
		bool loading() const { return m_log_job.has_value(); }
		// The last page came: the history has no more rows.
		bool at_end() const { return m_at_end; }

		// The selected commit's row, while it is among the rows loaded.
		std::optional<std::size_t> selected_row() const;
		const std::string& selected_commit() const { return m_selected; }
		const std::vector<history_file>& files() const { return m_files; }
		bool files_loading() const { return m_files_job.has_value(); }
		const std::string& selected_file() const { return m_selected_file; }
		// The unified diff of the selected file against the commit's first parent.
		const std::string& diff_text() const { return m_diff_text; }
		bool diff_truncated() const { return m_diff_truncated; }
		bool diff_loading() const { return m_diff_job.has_value(); }
		// Changes whenever diff_text() does.
		std::uint64_t diff_version() const { return m_diff_version; }

		// The last failure, as git or the command said it; also logged under "vcs".
		const std::string& error() const { return m_error; }

	private:
		enum class job_kind : std::uint8_t { log, files, diff };

		std::optional<std::uint64_t> start(std::string_view id, boost::json::value args, job_kind kind);
		void start_page();
		// Drops the rows and the running page, keeps the selection (it comes back when its row loads again).
		void restart();
		void clear_selection();
		void finished(std::uint64_t id, job_status status, const std::string& error, const std::optional<boost::json::value>& result);
		void apply_page(const boost::json::object& result);
		void forget_job(std::optional<std::uint64_t>& job);
		void fail(std::string text);

		editor_events& m_events;
		ports m_ports;
		std::map<std::uint64_t, job_kind> m_jobs;
		std::optional<std::uint64_t> m_log_job;
		std::optional<std::uint64_t> m_files_job;
		std::optional<std::uint64_t> m_diff_job;

		bool m_visible = false;
		bool m_loaded_once = false;
		// What the rows were asked with; m_filter waits for its quiet time to become it.
		history_filter m_applied_filter;
		history_filter m_filter;
		std::optional<clock::time_point> m_reload_at;

		std::vector<history_commit> m_commits;
		std::unordered_map<std::string, std::size_t> m_row_of;
		commit_graph m_graph;
		std::string m_cursor;
		bool m_at_end = false;
		// A page failed: no more pages until a reload or a new filter, so scrolling does not ask again and again.
		bool m_log_failed = false;
		std::size_t m_last_shown = 0;
		int m_restarts = 0;

		std::string m_selected;
		std::vector<history_file> m_files;
		std::string m_selected_file;
		std::string m_diff_text;
		bool m_diff_truncated = false;
		std::uint64_t m_diff_version = 0;

		std::string m_error;
		// Last: gone before the state its slot touches.
		connection m_finished;
	};
}
