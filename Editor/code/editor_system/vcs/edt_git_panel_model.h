#pragma once
#include "edt_history_model.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the Git panel shows and does (#456), without ImGui: every action is a vcs.* command (#454) run through
// `invoke` as a person's call, and its job's result comes back through editor_events::job_finished. Nothing runs
// while the panel is hidden; a change of code/ or of the repository (editor_events::repository_changed) seen meanwhile
// makes it refresh when it is shown. Main thread only.
namespace edt
{
	struct git_change
	{
		std::string path;
		// A renamed or copied file: where it came from.
		std::string orig_path;
		// git's XY: '.' unchanged, M, A, D, R, C, U, '?' untracked.
		char index = '.';
		char worktree = '.';
	};

	struct git_branch
	{
		// "main", or "origin/main" for a remote one.
		std::string name;
		bool remote = false;
		bool head = false;
	};

	struct git_diff_line
	{
		enum class kind : std::uint8_t { meta, hunk, context, added, removed, note };
		kind type = kind::context;
		// Without the line's end (a CR of a CRLF file included).
		std::string text;
	};

	// The latest progress the running write's job reported (fetch, pull, push: git's network progress).
	struct git_busy_progress
	{
		float fraction = 0.f;
		// "Receiving objects 45%".
		std::string text;
	};

	// A unified diff split into lines: the file headers (until the first hunk of each file) are meta, so a removed
	// line that reads "-- x" is not taken for a "--- a/x" header. Pure.
	std::vector<git_diff_line> diff_lines(std::string_view text);

	class git_panel_model
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
		};

		// A change of code/ refreshes after this quiet time.
		static constexpr std::chrono::milliseconds k_change_delay{ 1000 };
		// A change of the repository (editor_events::repository_changed) refreshes after this quiet time.
		static constexpr std::chrono::milliseconds k_repo_delay{ 250 };
		// A refs change refreshed this soon after a write of the panel ended is that write's own: writes_finished()
		// already reloaded the History tab.
		static constexpr std::chrono::milliseconds k_own_refs_window{ 2000 };

		git_panel_model(editor_events& events, ports ports);

		// The panel's frame (shown) or its hidden frames. Becoming visible refreshes when something changed meanwhile
		// or nothing was read yet.
		void set_visible(bool visible);
		// Runs a refresh a change of code/ asked for, once it is due.
		void tick();
		void refresh();
		// code/ changed: a refresh k_change_delay after the last change, while visible.
		void files_changed();
		// The repository changed: a refresh k_repo_delay after the last change, while visible; `refs` also moves
		// refs_changed() with that refresh. Raised through editor_events::repository_changed.
		void repository_changed(repository_change kind);

		void select(const std::string& path, bool staged);
		void set_staged(const std::string& path, bool staged);
		// The message being written; a commit clears it only once it succeeded.
		std::string& message() { return m_message; }
		// Something staged and a message that is not only blanks.
		bool can_commit() const;
		void commit();
		void fetch();
		void pull();
		// To the upstream's remote, else to origin with --set-upstream.
		void push();
		// A remote branch "origin/x" is checked out as "x" (git makes the tracking branch).
		void checkout(const git_branch& branch);
		void create_branch(const std::string& name);
		// The running fetch / pull / push / checkout / commit ... (job.cancel).
		void cancel();
		void clear_error() { m_error.clear(); }

		bool read() const { return m_read; }
		bool available() const { return m_available; }
		const std::string& unavailable_reason() const { return m_reason; }
		bool loading() const { return m_status_job.has_value(); }
		const std::string& head() const { return m_head; }
		bool detached() const { return m_detached; }
		const std::string& upstream() const { return m_upstream; }
		std::optional<std::int64_t> ahead() const { return m_ahead; }
		std::optional<std::int64_t> behind() const { return m_behind; }
		const std::vector<git_change>& staged() const { return m_staged; }
		const std::vector<git_change>& unstaged() const { return m_unstaged; }
		const std::vector<git_change>& untracked() const { return m_untracked; }
		bool has_changes() const { return !m_staged.empty() || !m_unstaged.empty() || !m_untracked.empty(); }
		// Changes a branch switch could get in the way of (untracked files are left where they are).
		bool has_tracked_changes() const { return !m_staged.empty() || !m_unstaged.empty(); }
		const std::vector<git_branch>& branches() const { return m_branches; }

		const std::string& selected() const { return m_selected; }
		bool selected_staged() const { return m_selected_staged; }
		const std::vector<git_diff_line>& diff() const { return m_diff; }
		bool diff_truncated() const { return m_diff_truncated; }
		// Why there is no diff to show (an untracked file), else empty.
		const std::string& diff_note() const { return m_diff_note; }

		// The write running now ("Push", "Commit" ...), empty when none.
		const std::string& busy() const { return m_busy; }
		// While the write runs and its job has reported progress; cleared when it ends.
		const std::optional<git_busy_progress>& busy_progress() const { return m_busy_progress; }
		// Writes that have ended, however: what HEAD and the refs point at may have moved (the History tab reloads).
		std::uint64_t writes_finished() const { return m_writes_finished; }
		// Refreshes that followed a change of HEAD or the refs not made by the panel's own write (that one moved
		// writes_finished()): the History tab reloads.
		std::uint64_t refs_changed() const { return m_refs_changed; }
		// The last failure, as git or the command said it; also logged under "vcs".
		const std::string& error() const { return m_error; }

	private:
		enum class job_kind : std::uint8_t { status, branches, diff, write };
		struct pending
		{
			job_kind kind = job_kind::status;
			std::string label;
		};

		std::optional<std::uint64_t> start(std::string_view id, boost::json::value args, job_kind kind, std::string label);
		void start_write(std::string_view id, boost::json::value args, std::string label);
		void finished(std::uint64_t id, job_status status, const std::string& error, const std::optional<boost::json::value>& result);
		void apply_status(const boost::json::object& result);
		// After a status: the selected file where it is now (staged if it is there and was, else where it moved), or
		// nothing when it is gone.
		void reselect();
		void forget();
		void apply_branches(const boost::json::object& result);
		void apply_diff(const boost::json::object& result);
		void fail(std::string text);

		editor_events& m_events;
		ports m_ports;
		std::map<std::uint64_t, pending> m_jobs;
		std::optional<std::uint64_t> m_status_job;
		std::optional<std::uint64_t> m_branches_job;
		std::optional<std::uint64_t> m_diff_job;
		std::optional<std::uint64_t> m_busy_job;

		bool m_visible = false;
		bool m_read = false;
		bool m_stale = true;
		std::optional<clock::time_point> m_refresh_at;
		// A refs change waits for the next refresh.
		bool m_refs_pending = false;
		std::uint64_t m_refs_changed = 0;
		// writes_finished() when a refs change was last taken.
		std::uint64_t m_writes_seen_by_refs = 0;
		std::optional<clock::time_point> m_last_write_end;

		bool m_available = false;
		std::string m_reason;
		std::string m_head;
		bool m_detached = false;
		std::string m_upstream;
		std::optional<std::int64_t> m_ahead;
		std::optional<std::int64_t> m_behind;
		std::vector<git_change> m_staged;
		std::vector<git_change> m_unstaged;
		std::vector<git_change> m_untracked;
		std::vector<git_branch> m_branches;

		std::string m_selected;
		bool m_selected_staged = false;
		std::vector<git_diff_line> m_diff;
		bool m_diff_truncated = false;
		std::string m_diff_note;

		std::string m_message;
		std::string m_busy;
		std::optional<git_busy_progress> m_busy_progress;
		std::uint64_t m_writes_finished = 0;
		std::string m_error;
		// Last: gone before the state their slots touch.
		connection m_finished;
		connection m_progress;
		connection m_repository;
	};

	// The History tab's ports: the panel's own, with vcs.log{all, author, grep}, vcs.show_files and vcs.diff{commit}.
	vcs::history_model::ports history_ports_of(const git_panel_model::ports& ports);
}
