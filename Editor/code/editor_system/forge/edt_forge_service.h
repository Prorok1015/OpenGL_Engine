#pragma once
#include "edt_forge_provider.h"
#include "edt_vcs_policy.h"
#include "edt_vcs_service.h"
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_registration.h>
#include <snk/platform/plt_secret.h>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace edt
{
	struct forge_options
	{
		// The provider of the code host the remotes point to (GitHub today).
		std::shared_ptr<forge::forge_provider> provider;
		// Where the tokens are kept, under the service `forge` and the host as the account. Its calls block: they run
		// inside the job, never on the main thread. Shared with the settings UI (#458 PR-4).
		std::shared_ptr<platform::secret_store> secrets;
		// The hosts of a remote's URL (the web host, lowercase: github.com) a request may go to. The transport has its
		// own allowlist of API hosts (editor/forge_hosts).
		std::vector<std::string> allowed_hosts{ "github.com" };
	};

	// Pull requests of the open project's code host (#458): create, status, list. Every call is a vcs_service reader job on
	// the io lane (job.cancel stops git and the request) whose result is the answer (job.get). owner and repository come
	// from the git remote and nowhere else; the token is read from the secret store inside the job and goes to the
	// transport only, so it is in no result, error, log or command. The secret store's calls can block until an OS prompt
	// is answered, which is why they run inside the job and never on the main thread. Main thread only.
	//
	// Hard rules of a call that is not a person's (origin agent, test, game), whatever the policy's mode: the pull request
	// is a draft (an explicit draft:false is refused `denied` before any job); the head branch exists locally, matches
	// push_branches and no protected_branches (vcs_push_branch_refusal); the remote is in the policy's remotes; the URL of
	// the remote parses and its host is allowed. That URL is the one written in the config (`remote.<name>.url`), never
	// `git remote get-url`'s: a remote with a pushurl, or whose URL a url.<base>.insteadOf rewrites, is refused `denied`
	// (the repository's own config could point the token at another repository). The repository is read before the
	// person is asked (describe_*) and the job checks that it found the same one.
	class forge_service
	{
	public:
		inline static constexpr std::size_t k_max_title = 256;
		inline static constexpr std::size_t k_max_body = 16 * 1024;
		inline static constexpr int k_max_list_limit = 50;
		inline static constexpr int k_default_list_limit = 30;
		// The service name of the secret store; the account is the host.
		inline static constexpr const char* k_secret_service = "forge";

		forge_service(vcs_service& vcs, forge_options options);
		~forge_service();
		forge_service(const forge_service&) = delete;
		forge_service& operator=(const forge_service&) = delete;

		// The agent policy's git rules, asked at the start of every call that is not a person's (as vcs_service's). Null:
		// vcs_policy's defaults.
		void set_policy_source(std::function<vcs_policy()> source) { m_policy_source = std::move(source); }

		struct create_request
		{
			std::string base;
			std::string head;
			std::string title;
			std::string body;
			// Only a person may ask for false.
			bool draft = true;
			bool draft_given = false;
			// Empty: git's default (the branch's remote, else the only one, else origin).
			std::string remote;
		};

		struct list_request
		{
			std::string state = "open";
			std::string head;
			std::string base;
			int limit = k_default_list_limit;
			std::string remote;
		};

		// What a person is told before an agent's call is allowed: the repository it would go to (host/owner/repo and the
		// remote), read now from the config on the main thread (bounded by vcs_options::probe_timeout), or why the call
		// will be refused. Remembers the repository, so that the job started for the same call (same session and
		// arguments) denies when it finds another. nullopt for a person's call. Plain text, already sanitized.
		std::optional<std::string> describe_create_pr(const create_request& request, const cmd::origin& caller);
		std::optional<std::string> describe_pr_status(std::int64_t number, const std::string& remote, const cmd::origin& caller);
		std::optional<std::string> describe_list_prs(const list_request& request, const cmd::origin& caller);

		vcs_service::started start_create_pr(const create_request& request, const cmd::origin& caller);
		vcs_service::started start_pr_status(std::int64_t number, const std::string& remote, const cmd::origin& caller);
		vcs_service::started start_list_prs(const list_request& request, const cmd::origin& caller);

	private:
		struct shown_target
		{
			std::string key;
			// "host/owner/repo (remote name)", or "refused" when the description was a refusal.
			std::string target;
			std::chrono::steady_clock::time_point at;
		};

		vcs_policy current_policy() const;
		std::optional<std::string> describe(std::string before, const std::string& remote, std::string key, const cmd::origin& caller,
			std::string after, std::optional<std::string> refusal);
		void remember_shown(std::string key, std::string target);
		// The repository shown for this call, once.
		std::optional<std::string> take_shown(const std::string& key);

		vcs_service& m_vcs;
		std::function<vcs_policy()> m_policy_source;
		// Held by the jobs as well: one dropped before it ran may outlive the service.
		std::shared_ptr<const forge_options> m_options;
		// The last few repositories shown to a person, by call; dropped when their job starts or after a few minutes.
		std::vector<shown_target> m_shown;
	};

	// forge.create_pr {base, head, title, body?, draft?, remote?} -> {job}; result: the pull request
	//     {number, title, state, draft, head, base, url}; draft is true unless a person says false
	// forge.pr_status {number, remote?} -> {job}; result: the pull request
	// forge.list_prs {state? (open|closed|all), head?, base?, limit? (1..50, 30), remote?} -> {job}; result {prs: [...]}
	// All three: category vcs_forge, ask by default (they use the token and the network); create_pr mutates and has
	// risk confirm. A failure ends the job `done` with result {error: {code, message}}: a forge_error_code name
	// (unauthorized, forbidden, rate_limited, not_found, validation, network, invalid_response, host_not_allowed,
	// unavailable), no_token (none stored: a person adds one in Settings), not_a_repository, vcs_unavailable, denied,
	// invalid_args, or what git reported. Setting or deleting a token is no command: only the settings UI does it.
	[[nodiscard]] std::vector<registration> register_forge_commands(cmd::command_registry& commands, forge_service& service);
}
