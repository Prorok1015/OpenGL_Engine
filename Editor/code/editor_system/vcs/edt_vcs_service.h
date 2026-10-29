#pragma once
#include "edt_toolchain.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_registration.h>
#include <chrono>
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
		// The probes (git --version, git rev-parse) run on the main thread: git is stopped after this long and version
		// control is off ("git did not answer within 5 s").
		std::chrono::milliseconds probe_timeout{ 5000 };
	};

	// What an agent's git write may run of the repository's config. Off: hooks point at an empty folder of the editor,
	// and a write that can run a filter driver (stage, unstage, commit, checkout, pull) is refused with
	// filters_require_opt_in (switching the driver off instead would commit LFS files as raw bytes).
	struct vcs_write_permissions
	{
		bool allow_hooks = false;
		bool allow_filters = false;
	};

	// Git for the editor and its agents (#454): reads (status, diff, log, branches), writes (stage, unstage, commit,
	// branch create, checkout) and the network (fetch, pull --ff-only, push). Every git command runs as
	// a job_service job on ds::lane::io (job.cancel stops git) and its parsed answer is the job's result (job.get).
	// Git runs with LC_ALL=C, no terminal or credential prompt (GIT_TERMINAL_PROMPT=0, GCM_INTERACTIVE=never, ssh in
	// BatchMode unless GIT_SSH_COMMAND is set), without the variables that redirect it or add config (GIT_DIR,
	// GIT_CONFIG_*, ...), and stderr read apart from stdout. A read never runs a program the repository's config names:
	// --no-pager, core.fsmonitor=false, core.hooksPath to nothing, diff.external empty, --no-ext-diff --no-textconv,
	// log.showSignature=false, submodules ignored, and every filter driver (listed per job, plus lfs) emptied.
	// A write keeps fsmonitor, diff.external and submodules off too; for a person (origin ui, test, game) hooks and
	// filters run as in plain git. For an agent they run only under vcs_write_permissions, and core.askPass and
	// core.sshCommand never come from the repository's config (GIT_ASKPASS and SSH_ASKPASS are dropped as well).
	// There is no force-push, reset --hard, branch delete or rebase, and never will be.
	// Main thread only.
	class vcs_service
	{
	public:
		struct state
		{
			bool available = false;
			// Why not: "git not found", "no project is open", "not a repository", ...
			std::string reason;
			// When not available: "vcs_unavailable" (no git, git cannot run, a probe timed out), "no_project", or what
			// git reported for the project root (vcs::classify_error: "not_a_repository", ...). Only the first two make
			// the commands refuse at once; otherwise their job runs and ends with git's error as its result.
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
		// stage / unstage: the paths of one call, in bytes (a command line on Windows holds 32K characters).
		inline static constexpr std::size_t k_max_paths_bytes = 16 * 1024;

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

		// For the jobs started after it. TODO(#455): fed from the project's agent policy; until then both stay off.
		void set_write_permissions(vcs_write_permissions permissions);
		const vcs_write_permissions& write_permissions() const { return m_write_permissions; }

		// Writes; `caller` is the origin of the command (cmd::call::from). Paths are relative to the repository root.
		started start_stage(const std::vector<std::string>& paths, cmd::origin_kind caller);
		started start_unstage(const std::vector<std::string>& paths, cmd::origin_kind caller);
		started start_commit(const std::string& message, cmd::origin_kind caller);
		// `from` empty: HEAD.
		started start_branch_create(const std::string& name, const std::string& from, cmd::origin_kind caller);
		started start_checkout(const std::string& ref, cmd::origin_kind caller);
		// `remote` empty: git's default (the branch's remote, else origin).
		started start_fetch(const std::string& remote, cmd::origin_kind caller);
		// `branch` needs a `remote`.
		started start_pull(const std::string& remote, const std::string& branch, cmd::origin_kind caller);
		started start_push(const std::string& remote, const std::string& branch, bool set_upstream, cmd::origin_kind caller);

	private:
		// A folder in the OS temp dir that only this editor writes to: the empty hooks folder of agent writes and the
		// commit message files. Removed with the service, after its jobs.
		struct scratch_dir
		{
			std::filesystem::path root;
			scratch_dir() = default;
			scratch_dir(const scratch_dir&) = delete;
			scratch_dir& operator=(const scratch_dir&) = delete;
			~scratch_dir();
		};

		struct write_mode
		{
			cmd::origin_kind caller = cmd::origin_kind::ui;
			// The command reads or writes the work tree, where filter drivers run.
			bool runs_filters = false;
		};

		void probe_repository(const std::optional<std::filesystem::path>& project);
		void set_probe_timeout();
		started unavailable() const;
		// The scratch folder with an empty hooks folder in it, made on first use; the error text when it cannot be.
		std::optional<std::string> ensure_scratch();
		// Defined in the .cpp, its only user. `write`: nullopt for a read.
		template<class F>
		started run_job(std::string label, F work, std::optional<write_mode> write = std::nullopt);

		job_service& m_jobs;
		vcs_options m_options;
		std::vector<std::string> m_environment;
		state m_state;
		std::optional<std::filesystem::path> m_probed_project;
		bool m_probed = false;
		bool m_git_found = false;
		vcs_write_permissions m_write_permissions;
		scratch_dir m_scratch;
		std::uint64_t m_message_count = 0;
		// Last: its jobs are stopped before the rest goes.
		ds::lifetime m_life{ ds::lifetime_option::cancel_on_destroy };
	};

	// vcs.info {} -> {available, reason?, code?, root?, git_version?} (sync)
	// vcs.status {} -> {job}; result {branch: {head, detached, oid, upstream?, ahead?, behind?},
	//     entries: [{path, orig_path?, index, worktree, kind: ordinary|renamed|unmerged|untracked|ignored}], clean, truncated}
	// vcs.diff {path?, staged?} -> {job}; result {text, truncated} (text cut at 256 KiB, truncated true then)
	// vcs.log {ref?, path?, limit? (1..200, 50), cursor?} -> {job}; result {commits: [{hash, parents, author, email,
	//     time, subject}], next_cursor?, truncated}
	// vcs.branches {} -> {job}; result {current?, branches: [{name, full_name, oid, upstream?, head, remote}], truncated}
	// status, log and branches stop git past 4 MiB of output and keep the whole records (truncated: true).
	// Writes (mutates; no agent category until #455, so agents see none of them):
	// vcs.stage {paths} / vcs.unstage {paths} -> {job}; result {output}
	// vcs.commit {message} -> {job}; result {hash}
	// vcs.branch.create {name, from?} / vcs.checkout {ref} -> {job}; result {output}
	// vcs.fetch {remote?} / vcs.pull {remote?, branch?} (--ff-only) -> {job}; result {output}
	// vcs.push {remote, branch, set_upstream?} -> {job}; result {output} (no force: a branch +x, -x or a:b is invalid_args)
	// `output` is git's last lines (at most 20, 4 KiB).
	// A git failure ends the job `done` with result {error: {code, message, stderr_tail, exit_code}}: code from
	// vcs::classify_error (auth_required, not_a_repository, ...) or vcs_unavailable when git could not start, or
	// filters_require_opt_in for an agent's write refused before git ran; message is git's last fatal:/error: line;
	// stderr_tail its last 20 stderr lines (at most 4 KiB).
	// Only without git (vcs_unavailable) or a project (no_project) do the commands fail at once.
	[[nodiscard]] std::vector<registration> register_vcs_commands(cmd::command_registry& commands, vcs_service& service);
}
