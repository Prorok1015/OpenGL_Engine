#pragma once
#include "edt_toolchain.h"
#include "edt_vcs_policy.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_registration.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// vcs.log's filters besides ref and path. At namespace scope: GCC cannot use a nested class with default member
	// initializers as a default argument of its enclosing class's member functions.
	struct vcs_log_filter
	{
		// Every branch, remote branch and tag instead of one ref.
		bool all = false;
		// Literal, without case; empty: none.
		std::string author;
		std::string grep;
	};

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

	// Git for the editor and its agents (#454): reads (status, diff, log, show_files, branches), writes (stage, unstage, commit,
	// branch create, checkout) and the network (fetch, pull --ff-only, push). Every git command runs as
	// a job_service job on ds::lane::io (job.cancel stops git) and its parsed answer is the job's result (job.get).
	// Git runs with LC_ALL=C, no terminal or credential prompt (GIT_TERMINAL_PROMPT=0, GCM_INTERACTIVE=never, ssh in
	// BatchMode unless GIT_SSH_COMMAND is set), without the variables that redirect it or add config (GIT_DIR,
	// GIT_CONFIG_*, ...), and stderr read apart from stdout. A read never runs a program the repository's config names:
	// --no-pager, core.fsmonitor=false, core.hooksPath to nothing, diff.external empty, --no-ext-diff --no-textconv,
	// log.showSignature=false, submodules ignored, and every filter driver (listed per job, plus lfs) emptied.
	// A write keeps fsmonitor, diff.external and submodules off too; for a person (origin ui) hooks and filters run as in
	// plain git. Every other origin (agent, test, game) is guarded: the repository's own config must pass an allowlist,
	// hooks and filters run only under vcs_write_permissions, only https and ssh transports are allowed, and
	// core.askPass, core.sshCommand, core.gitProxy, core.alternateRefsCommand and signing are switched off
	// (GIT_ASKPASS and SSH_ASKPASS dropped); a network call is refused (unsafe_repository) when the config sets a
	// credential helper, core.sshCommand, a URL rewrite to another transport, or an upload-pack / receive-pack.
	// Hard rules of a guarded write (vcs_policy, #455), checked before git runs whatever the policy's mode: a push goes
	// only to a branch matching push_branches and no protected branch, as the explicit refspec refs/heads/<b>:refs/heads/<b>
	// without tags; fetch, pull and push only to a remote in remotes (an omitted remote is resolved once, as git would,
	// and named to git; a pull without a branch fetches the current branch's upstream by name); a checkout or pull whose
	// target changes the project's agent policy file is refused; a refusal is a `done` job with error code `denied`.
	// A guarded commit loses every Agent-Session line of its message and gets `Agent-Session: <session>` for a valid
	// agent session.
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
			// Absolute: the worktree's own .git folder (HEAD, index) and the folder shared by all worktrees (refs,
			// packed-refs). The same folder outside a linked worktree.
			std::optional<std::filesystem::path> git_dir;
			std::optional<std::filesystem::path> common_dir;
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

		// Finds git and the repository right away (git --version, git rev-parse --show-toplevel --absolute-git-dir
		// --git-common-dir): the answer of
		// vcs.info. Nothing fails here: an editor without git keeps running.
		vcs_service(job_service& jobs, vcs_options options);
		~vcs_service();
		vcs_service(const vcs_service&) = delete;
		vcs_service& operator=(const vcs_service&) = delete;

		// The cached state; the repository is looked up again when the project root changed.
		const state& info();
		// The project root the cached state was probed for.
		const std::optional<std::filesystem::path>& probed_project() const { return m_probed_project; }

		// Called on the main thread as each job that runs git is started, before git can run (repository_watcher:
		// the work tree and the index changes of the editor's own git calls are not news).
		void set_job_observer(std::function<void()> on_start) { m_job_observer = std::move(on_start); }
		// The jobs that run git and have not ended yet (any thread decrements it as a job ends or is dropped unrun).
		int running_jobs() const { return m_running_jobs->load(); }

		using log_filter = vcs_log_filter;

		inline static constexpr std::size_t k_max_log_snapshots = 8;
		inline static constexpr std::size_t k_max_filter_bytes = 256;

		// Reads; `caller` is the origin of the command (cmd::call::from). Not a person's (ui): no lazy fetch, no
		// transport, and before git 2.45 a partial clone is refused.
		started start_status(const cmd::origin& caller);
		// `path` empty: the whole tree. `staged`: the index against HEAD.
		started start_diff(const std::string& path, bool staged, const cmd::origin& caller);
		// The change `commit` (4..64 hex digits) made to `path` (empty: every file), against its first parent. With a
		// path, a file the commit renamed shows as added: rename detection sees only the files of the path.
		started start_commit_diff(const std::string& commit, const std::string& path, const cmd::origin& caller);
		// `ref` empty: HEAD. `limit` is clamped to [1, k_max_log_limit]. `cursor` is the next_cursor of a page (empty:
		// the first page). The first page resolves its tips to commits once and keeps them as a snapshot (at most
		// k_max_log_snapshots, least recently used dropped), so later pages walk the same history whatever was
		// committed meanwhile; the cursor of a dropped snapshot, or of other arguments, ends the job with error `stale`.
		started start_log(const std::string& ref, const std::string& path, int limit, const std::string& cursor,
			const log_filter& filter, const cmd::origin& caller);
		// The files `commit` (4..64 hex digits) changed, against its first parent (a root commit: every file).
		started start_show_files(const std::string& commit, const cmd::origin& caller);
		started start_branches(const cmd::origin& caller);

		// The same, by kind only (no agent session).
		started start_status(cmd::origin_kind caller) { return start_status(cmd::origin{ caller, {} }); }
		started start_diff(const std::string& path, bool staged, cmd::origin_kind caller)
		{
			return start_diff(path, staged, cmd::origin{ caller, {} });
		}
		started start_commit_diff(const std::string& commit, const std::string& path, cmd::origin_kind caller)
		{
			return start_commit_diff(commit, path, cmd::origin{ caller, {} });
		}
		started start_log(const std::string& ref, const std::string& path, int limit, const std::string& cursor,
			const log_filter& filter, cmd::origin_kind caller)
		{
			return start_log(ref, path, limit, cursor, filter, cmd::origin{ caller, {} });
		}
		// Without author, grep or all.
		started start_log(const std::string& ref, const std::string& path, int limit, const std::string& cursor,
			const cmd::origin& caller)
		{
			return start_log(ref, path, limit, cursor, log_filter{}, caller);
		}
		started start_log(const std::string& ref, const std::string& path, int limit, const std::string& cursor,
			cmd::origin_kind caller)
		{
			return start_log(ref, path, limit, cursor, log_filter{}, cmd::origin{ caller, {} });
		}
		started start_show_files(const std::string& commit, cmd::origin_kind caller)
		{
			return start_show_files(commit, cmd::origin{ caller, {} });
		}
		started start_branches(cmd::origin_kind caller) { return start_branches(cmd::origin{ caller, {} }); }

		// For the jobs started after it, while no policy source is set (the hard rules then use vcs_policy's defaults).
		void set_write_permissions(vcs_write_permissions permissions);
		const vcs_write_permissions& write_permissions() const { return m_write_permissions; }
		// The agent policy's git rules, asked at the start of every guarded write, so an edit of the policy file applies
		// at once; it replaces set_write_permissions. Null: the defaults and set_write_permissions.
		void set_policy_source(std::function<vcs_policy()> source) { m_policy_source = std::move(source); }
		// The project's agent policy file, which a guarded checkout or pull must not change; empty (the default):
		// <project root>/.snk/agent_policy.json.
		void set_policy_file(std::filesystem::path file) { m_policy_file = std::move(file); }

		// Writes; `caller` is the origin of the command (cmd::call::from). Paths are relative to the repository root.
		started start_stage(const std::vector<std::string>& paths, const cmd::origin& caller);
		started start_unstage(const std::vector<std::string>& paths, const cmd::origin& caller);
		started start_commit(const std::string& message, const cmd::origin& caller);
		// `from` empty: HEAD.
		started start_branch_create(const std::string& name, const std::string& from, const cmd::origin& caller);
		started start_checkout(const std::string& ref, const cmd::origin& caller);
		// `remote` empty: git's default (the branch's remote, else origin).
		started start_fetch(const std::string& remote, const cmd::origin& caller);
		// `branch` needs a `remote`.
		started start_pull(const std::string& remote, const std::string& branch, const cmd::origin& caller);
		started start_push(const std::string& remote, const std::string& branch, bool set_upstream, const cmd::origin& caller);

		// The same, by kind only (no agent session).
		started start_stage(const std::vector<std::string>& paths, cmd::origin_kind caller) { return start_stage(paths, cmd::origin{ caller, {} }); }
		started start_unstage(const std::vector<std::string>& paths, cmd::origin_kind caller) { return start_unstage(paths, cmd::origin{ caller, {} }); }
		started start_commit(const std::string& message, cmd::origin_kind caller) { return start_commit(message, cmd::origin{ caller, {} }); }
		started start_branch_create(const std::string& name, const std::string& from, cmd::origin_kind caller)
		{
			return start_branch_create(name, from, cmd::origin{ caller, {} });
		}
		started start_checkout(const std::string& ref, cmd::origin_kind caller) { return start_checkout(ref, cmd::origin{ caller, {} }); }
		started start_fetch(const std::string& remote, cmd::origin_kind caller) { return start_fetch(remote, cmd::origin{ caller, {} }); }
		started start_pull(const std::string& remote, const std::string& branch, cmd::origin_kind caller)
		{
			return start_pull(remote, branch, cmd::origin{ caller, {} });
		}
		started start_push(const std::string& remote, const std::string& branch, bool set_upstream, cmd::origin_kind caller)
		{
			return start_push(remote, branch, set_upstream, cmd::origin{ caller, {} });
		}

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
			// The command talks to a remote.
			bool network = false;
		};
		struct log_snapshots;
		// A hard rule of a guarded write: why the policy refuses it, nullopt when it does not.
		using hard_rule = std::function<std::optional<std::string>(const vcs_policy&)>;

		void probe_repository(const std::optional<std::filesystem::path>& project);
		void set_probe_timeout();
		started unavailable() const;
		// The scratch folder with an empty hooks folder in it, made on first use; the error text when it cannot be.
		std::optional<std::string> ensure_scratch();
		// Defined in the .cpp, its only user. `write`: nullopt for a read, whose caller is `reader` (not ui: the
		// protected read).
		template<class F>
		started run_job(std::string label, F work, std::optional<write_mode> write = std::nullopt, const hard_rule& rule = {},
			cmd::origin_kind reader = cmd::origin_kind::agent);
		vcs_policy current_policy() const;

		job_service& m_jobs;
		vcs_options m_options;
		std::vector<std::string> m_environment;
		state m_state;
		std::optional<std::filesystem::path> m_probed_project;
		bool m_probed = false;
		bool m_git_found = false;
		vcs_write_permissions m_write_permissions;
		std::function<vcs_policy()> m_policy_source;
		std::filesystem::path m_policy_file;
		scratch_dir m_scratch;
		std::uint64_t m_message_count = 0;
		// Shared with the log jobs, which fill it on the io lane.
		std::shared_ptr<log_snapshots> m_log_snapshots;
		std::function<void()> m_job_observer;
		// Shared with the jobs: one dropped before it ran may be destroyed after the service.
		std::shared_ptr<std::atomic<int>> m_running_jobs = std::make_shared<std::atomic<int>>(0);
		// Last: its jobs are stopped before the rest goes.
		ds::lifetime m_life{ ds::lifetime_option::cancel_on_destroy };
	};

	// vcs.info {} -> {available, reason?, code?, root?, git_dir?, common_dir?, git_version?} (sync)
	// vcs.status {} -> {job}; result {branch: {head, detached, oid, upstream?, ahead?, behind?},
	//     entries: [{path, orig_path?, index, worktree, kind: ordinary|renamed|unmerged|untracked|ignored}], clean, truncated}
	// vcs.diff {path?, staged?, commit?} -> {job}; result {text, truncated} (text cut at 256 KiB, truncated true then);
	//     with commit: that commit's change against its first parent
	// vcs.log {ref?, all?, path?, author?, grep?, limit? (1..200, 50), cursor?} -> {job}; result {commits: [{hash,
	//     parents, author, email, time, subject, refs: [{name, kind: head|branch|remote|tag}]}], next_cursor?,
	//     truncated}; newest first by commit date (--date-order); a cursor whose snapshot is gone: error `stale`
	// vcs.show_files {commit} -> {job}; result {files: [{path, status, orig_path?}], truncated}
	// vcs.branches {} -> {job}; result {current?, branches: [{name, full_name, oid, upstream?, head, remote}], truncated}
	// status, log and branches stop git past 4 MiB of output and keep the whole records (truncated: true).
	// Writes (mutates; agent categories vcs_commit, vcs_branch, vcs_sync, vcs_push; reads are vcs_read):
	// vcs.stage {paths} / vcs.unstage {paths} -> {job}; result {output}
	// vcs.commit {message} -> {job}; result {hash}
	// vcs.branch.create {name, from?} / vcs.checkout {ref} -> {job}; result {output}
	// vcs.fetch {remote?} / vcs.pull {remote?, branch?} -> {job}; result {output} (pull = fetch, then
	//     merge --ff-only FETCH_HEAD)
	// vcs.push {remote, branch, set_upstream?} -> {job}; result {output} (no force: a branch +x, -x or a:b is invalid_args)
	// `output` is git's last lines (at most 20, 4 KiB).
	// A git failure ends the job `done` with result {error: {code, message, stderr_tail, exit_code}}: code from
	// vcs::classify_error (auth_required, not_a_repository, ...) or vcs_unavailable when git could not start, or
	// filters_require_opt_in / unsafe_repository / denied (a hard rule of vcs_policy) for an agent's write refused before
	// git ran; message is git's last fatal:/error: line;
	// stderr_tail its last 20 stderr lines (at most 4 KiB).
	// Only without git (vcs_unavailable) or a project (no_project) do the commands fail at once.
	[[nodiscard]] std::vector<registration> register_vcs_commands(cmd::command_registry& commands, vcs_service& service);
}
