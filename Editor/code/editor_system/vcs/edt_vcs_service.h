#pragma once
#include "edt_toolchain.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_registration.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	struct vcs_options
	{
		// cfg vcs/git; empty: "git" on the PATH of the editor's environment.
		std::filesystem::path git;
		// The open project's root; nullopt (or null): no project.
		std::function<std::optional<std::filesystem::path>()> project_root;
		process_runner run = platform::run_process;
		// The environment git runs in, before the vcs variables are added; nullopt: this process's.
		std::optional<std::vector<std::string>> environment;
	};

	// Git for the editor and its agents (#454). Read-only here: status, diff, log, branches. Every git command runs as
	// a job_service job on ds::lane::io (job.cancel stops git) and its parsed answer is the job's result (job.get).
	// Git runs with LC_ALL=C, no terminal or credential prompt (GIT_TERMINAL_PROMPT=0, GCM_INTERACTIVE=never, ssh in
	// BatchMode unless GIT_SSH_COMMAND is set), `-c core.quotepath=off --no-optional-locks --literal-pathspecs`, and
	// stderr read apart from stdout. There is no force-push, reset --hard, branch delete or rebase, and never will be.
	// Main thread only.
	class vcs_service
	{
	public:
		struct state
		{
			bool available = false;
			// Why not: "git not found", "no project", "not a repository", ...
			std::string reason;
			// "no_project", "vcs_unavailable" or "not_a_repository" when not available.
			std::string code;
			std::filesystem::path git;
			std::string git_version;
			// The repository's top level (git rev-parse --show-toplevel).
			std::optional<std::filesystem::path> root;
		};

		// What a start_* call did: a job, or why there is none (code + message, as a command failure).
		struct started
		{
			job_service::job_id job = 0;
			std::string code;
			std::string error;

			explicit operator bool() const { return code.empty(); }
		};

		inline static constexpr std::size_t k_diff_cap = 256 * 1024;
		inline static constexpr int k_max_log_limit = 200;
		inline static constexpr int k_default_log_limit = 50;

		// Finds git and the repository right away (git --version, git rev-parse --show-toplevel): the answer of
		// vcs.info. Nothing fails here: an editor without git keeps running.
		vcs_service(job_service& jobs, vcs_options options);
		~vcs_service();
		vcs_service(const vcs_service&) = delete;
		vcs_service& operator=(const vcs_service&) = delete;

		// The cached state; the repository is looked up again when the project root changed.
		const state& info();

		started start_status();
		// `path` empty: the whole tree. `staged`: the index against HEAD.
		started start_diff(const std::string& path, bool staged);
		// `ref` empty: HEAD. `limit` is clamped to [1, k_max_log_limit]. `cursor` is the next_cursor of a page (empty:
		// the first page); it counts commits from the newest, so a commit made meanwhile shifts the pages by one.
		started start_log(const std::string& ref, const std::string& path, int limit, const std::string& cursor);
		started start_branches();

	private:
		void probe_repository(const std::optional<std::filesystem::path>& project);
		started unavailable() const;
		// Defined in the .cpp, its only user.
		template<class F>
		started run_job(std::string label, F work);

		job_service& m_jobs;
		vcs_options m_options;
		std::vector<std::string> m_environment;
		state m_state;
		std::optional<std::filesystem::path> m_probed_project;
		bool m_probed = false;
		bool m_git_found = false;
		// Last: its jobs are stopped before the rest goes.
		ds::lifetime m_life{ ds::lifetime_option::cancel_on_destroy };
	};

	// vcs.info {} -> {available, reason?, code?, root?, git_version?} (sync)
	// vcs.status {} -> {job}; result {branch: {head, detached, oid, upstream?, ahead?, behind?},
	//     entries: [{path, orig_path?, index, worktree, kind: ordinary|renamed|unmerged|untracked|ignored}], clean}
	// vcs.diff {path?, staged?} -> {job}; result {text, truncated} (text cut at 256 KiB, truncated true then)
	// vcs.log {ref?, path?, limit? (1..200, 50), cursor?} -> {job}; result {commits: [{hash, parents, author, email,
	//     time, subject}], next_cursor?}
	// vcs.branches {} -> {job}; result {current?, branches: [{name, full_name, oid, upstream?, head, remote}]}
	// A git failure ends the job `done` with result {error: {code, message, exit_code}}; code from
	// vcs::classify_error (auth_required, not_a_repository, ...) or vcs_unavailable when git could not start.
	// Without git or a repository the commands fail at once with vcs_unavailable / no_project / not_a_repository.
	[[nodiscard]] std::vector<registration> register_vcs_commands(cmd::command_registry& commands, vcs_service& service);
}
