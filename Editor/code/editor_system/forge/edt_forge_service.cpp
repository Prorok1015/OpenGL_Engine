#include "edt_forge_service.h"
#include "edt_forge_types.h"
#include <snk/command/cmd_json_schema.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {

	// Longest title, branch name or URL a result carries.
	constexpr std::size_t k_text_cap = 512;

	json::value error_result(std::string_view code, std::string_view message, const platform::secret_text* token = nullptr)
	{
		std::string text = edt::forge::sanitize_text(message, edt::forge::k_max_error_message);
		if (token != nullptr) edt::forge::scrub_secret(text, *token);
		return json::object{ { "error", json::object{ { "code", json::string(std::string(code)) }, { "message", json::string(text) } } } };
	}

	// A field of a result: text from the server, safe to show, and without the token even if the provider missed it.
	json::string field(std::string_view text, const platform::secret_text& token)
	{
		std::string safe = edt::forge::sanitize_text(text, k_text_cap);
		edt::forge::scrub_secret(safe, token);
		return json::string(safe);
	}

	json::value pr_json(const edt::forge::pr_info& pr, const platform::secret_text& token)
	{
		return json::object{
			{ "number", pr.number },
			{ "title", field(pr.title, token) },
			{ "state", field(pr.state, token) },
			{ "draft", pr.draft },
			{ "head", field(pr.head, token) },
			{ "base", field(pr.base, token) },
			{ "url", field(pr.url, token) } };
	}

	json::value forge_failure(const edt::forge::forge_error& error, const platform::secret_text& token)
	{
		return error_result(edt::forge::forge_error_code_name(error.code), error.message, &token);
	}

	// What a call needs besides the provider's own arguments; copied into the job.
	struct call_plan
	{
		std::string label;
		// As the caller named it; empty: git's default.
		std::string remote;
		// Not a person's call: the hard rules apply.
		bool guarded = false;
		edt::vcs_policy policy;
		// create_pr, guarded: the head branch, which passes the push rules and exists locally.
		std::string head;
		// Guarded: the repository a person was shown for this very call ("host/owner/repo (remote name)"); the job finds
		// the same or denies. Empty: nobody was asked (the policy allowed it).
		std::optional<std::string> shown;
		// Guarded: a person approved this call, so a missing `shown` is a record lost, not a call nobody was asked about.
		bool approved = false;
	};

	struct target
	{
		edt::forge::forge_remote remote;
		platform::secret_text token;
	};

	bool host_allowed(const edt::forge_options& options, std::string_view host)
	{
		return std::any_of(options.allowed_hosts.begin(), options.allowed_hosts.end(), [host](const std::string& allowed) {
			if (allowed.size() != host.size()) return false;
			for (std::size_t i = 0; i < host.size(); ++i) {
				const auto a = static_cast<unsigned char>(allowed[i]);
				const auto b = static_cast<unsigned char>(host[i]);
				if (std::tolower(a) != std::tolower(b)) return false;
			}
			return true;
		});
	}

	struct url_failure
	{
		std::string code;
		std::string message;
	};

	// The remote's URL as the rules see it: parses, and its host is allowed. `out` is filled when nullopt comes back.
	std::optional<url_failure> judge_url(const edt::forge_options& options, bool guarded, const std::string& remote_name,
		std::string_view url, edt::forge::forge_remote& out)
	{
		auto parsed = edt::forge::parse_forge_remote(url);
		if (!parsed) return url_failure{ edt::forge::forge_error_code_name(parsed.error().code), parsed.error().message };
		if (!host_allowed(options, parsed.value().host)) {
			return url_failure{ guarded ? "denied" : "host_not_allowed",
				std::format("the host '{}' of the remote '{}' is not one pull requests are made on", parsed.value().host, remote_name) };
		}
		out = std::move(parsed.value());
		return std::nullopt;
	}

	// How the repository of a call is named to a person and compared by the job.
	std::string target_text(const edt::forge::forge_remote& remote, const std::string& remote_name)
	{
		return std::format("{}/{}/{} (remote {})", remote.host, remote.owner, remote.repo, remote_name);
	}

	bool remote_in_policy(const edt::vcs_policy& policy, const std::string& name)
	{
		return std::find(policy.remotes.begin(), policy.remotes.end(), name) != policy.remotes.end();
	}

	// Everything up to the network: the rules, the remote and its URL, the token. The job's answer when something is wrong;
	// nullopt with `out` filled when the call may go on. Runs on the io lane (the secret store blocks).
	std::optional<json::value> prepare(const edt::forge_options& options, const call_plan& plan,
		const edt::vcs_service::repository_reader& git, ds::task_context& task, target& out)
	{
		const std::stop_token stop = task.stop;
		// Fail closed: without the repository the person was shown there is nothing to compare the one found with.
		if (plan.guarded && plan.approved && !plan.shown) {
			return error_result("denied", "the repository shown for approval is no longer known: ask again");
		}
		if (plan.guarded && !plan.head.empty()) {
			if (auto why = edt::vcs_push_branch_refusal(plan.policy, plan.head)) {
				return error_result("denied", "a pull request's head must be a branch an agent may push: " + *why);
			}
		}
		const auto name = git.remote_name(plan.remote, stop);
		if (!name.ok()) return error_result(name.code, name.error);
		if (plan.guarded && !remote_in_policy(plan.policy, name.text)) {
			return error_result("denied", std::format("the agent policy does not allow the remote '{}' (vcs.remotes)", name.text));
		}
		// A guarded call takes the URL as written, not `git remote get-url`'s (insteadOf applied, pushurl ignored): it is the
		// one a person was shown.
		const auto url = plan.guarded ? git.remote_raw_url(name.text, stop) : git.remote_url(name.text, stop);
		if (!url.ok()) return error_result(url.code, url.error);
		edt::forge::forge_remote parsed;
		if (auto failure = judge_url(options, plan.guarded, name.text, url.text, parsed)) return error_result(failure->code, failure->message);
		if (plan.guarded && plan.shown) {
			const std::string found = target_text(parsed, name.text);
			if (found != *plan.shown) {
				return error_result("denied", std::format("the repository is not the one the request was allowed for (allowed for '{}', found '{}')",
					*plan.shown, found));
			}
		}
		if (plan.guarded && !plan.head.empty()) {
			const auto branch = git.local_branch(plan.head, stop);
			if (!branch.ok()) {
				return error_result(branch.code == cmd::codes::not_found ? "no_local_branch" : branch.code, branch.error);
			}
		}

		const platform::secret_key key{ edt::forge_service::k_secret_service, parsed.host };
		const platform::secret_result loaded = options.secrets ? options.secrets->load(key, out.token)
			: platform::secret_result{ platform::secret_status::unavailable, "no secret store" };
		if (stop.stop_requested()) throw ds::task_cancelled{};
		if (loaded.status == platform::secret_status::not_found || (loaded.status == platform::secret_status::ok && out.token.empty())) {
			return error_result("no_token", std::format("no token is stored for {}: add one in Settings (Version Control)", parsed.host));
		}
		if (loaded.status != platform::secret_status::ok) {
			return error_result(edt::forge::forge_error_code_name(edt::forge::forge_error_code::unavailable),
				"the secret store could not be read: " + loaded.message);
		}
		egLOG_DEBUG("forge", "{} on {}", plan.label, parsed.host);
		out.remote = std::move(parsed);
		return std::nullopt;
	}

	// `call(provider, target, stop)` is the network step; its answer is the job's.
	template <class F>
	edt::vcs_service::started start_call(edt::vcs_service& vcs, std::shared_ptr<const edt::forge_options> options, call_plan plan,
		const cmd::origin& caller, F call)
	{
		std::string label = plan.label;
		return vcs.start_reader_job(std::move(label), [options = std::move(options), plan = std::move(plan), call = std::move(call)](
			const edt::vcs_service::repository_reader& git, ds::task_context& task) -> json::value {
			target found;
			if (auto failure = prepare(*options, plan, git, task, found)) return std::move(*failure);
			if (!options->provider) return error_result("unavailable", "no code host is configured");
			json::value answer = call(*options->provider, found, task.stop);
			// A stopped request comes back as a network error: the job is cancelled, not failed.
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			return answer;
		}, caller);
	}

	std::optional<std::string> request_error(const edt::forge_service::create_request& request)
	{
		if (!edt::forge::is_valid_branch_name(request.base)) return std::string("base is not a valid branch name");
		if (!edt::forge::is_valid_branch_name(request.head)) return std::string("head is not a valid branch name");
		if (request.title.empty() || request.title.size() > edt::forge_service::k_max_title) {
			return std::format("title must be 1 to {} bytes", edt::forge_service::k_max_title);
		}
		// Nothing a title or a body can hide behind: it is what a reviewer reads, and it is shown to a person.
		if (!edt::forge::is_plain_text(request.title, false)) {
			return std::string("title must be valid UTF-8 without control, bidirectional or invisible characters");
		}
		if (request.body.size() > edt::forge_service::k_max_body) {
			return std::format("body must be at most {} bytes", edt::forge_service::k_max_body);
		}
		if (!edt::forge::is_plain_text(request.body, true)) {
			return std::string("body must be valid UTF-8 without control characters (except tab and line breaks), "
				"bidirectional or invisible characters");
		}
		return std::nullopt;
	}

	constexpr const char* JOB_RESULT_SCHEMA = R"({
		"type": "object",
		"properties": { "job": { "type": "integer", "minimum": 0 } },
		"required": ["job"],
		"additionalProperties": false
	})";

	cmd::result job_answer(const edt::vcs_service::started& started)
	{
		if (!started) return cmd::result::failure(started.code, started.error);
		return cmd::result::success(json::object{ { "job", static_cast<std::int64_t>(started.job) } });
	}

	std::string string_arg(const json::object& args, std::string_view name)
	{
		if (const json::value* value = args.if_contains(name)) return std::string(value->as_string());
		return {};
	}

	cmd::command_desc forge_command(std::string id, std::string summary, const char* args_schema, bool mutates)
	{
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = json::parse(args_schema);
		desc.result_schema = json::parse(JOB_RESULT_SCHEMA);
		desc.starts_job = true;
		desc.mutates = mutates;
		desc.category = cmd::agent_category::vcs_forge;
		return desc;
	}

	// Identifies one call (what the person was asked about) among those shown: the session and every argument, each with its length.
	std::string call_key(const cmd::origin& caller, std::string_view kind, std::initializer_list<std::string_view> parts)
	{
		std::string key = std::format("{}:{}|{}:{}", caller.session.size(), caller.session, kind.size(), kind);
		for (const std::string_view part : parts) key += std::format("|{}:{}", part.size(), part);
		return key;
	}

	constexpr std::size_t k_max_shown = 32;
	constexpr std::chrono::minutes k_shown_lifetime{ 5 };

	edt::forge_service::create_request create_request_of(const json::object& args)
	{
		edt::forge_service::create_request wanted;
		wanted.base = string_arg(args, "base");
		wanted.head = string_arg(args, "head");
		wanted.title = string_arg(args, "title");
		wanted.body = string_arg(args, "body");
		wanted.remote = string_arg(args, "remote");
		if (const json::value* draft = args.if_contains("draft")) {
			wanted.draft = draft->as_bool();
			wanted.draft_given = true;
		}
		return wanted;
	}

	edt::forge_service::list_request list_request_of(const json::object& args)
	{
		edt::forge_service::list_request wanted;
		if (const json::value* state = args.if_contains("state")) wanted.state = std::string(state->as_string());
		wanted.head = string_arg(args, "head");
		wanted.base = string_arg(args, "base");
		wanted.remote = string_arg(args, "remote");
		if (const json::value* limit = args.if_contains("limit")) {
			wanted.limit = static_cast<int>(cmd::json_schema::integer_of(*limit).value_or(edt::forge_service::k_default_list_limit));
		}
		return wanted;
	}

	void keep(std::vector<edt::registration>& out, edt::registration added)
	{
		if (added.active()) out.push_back(std::move(added));
	}
}

edt::forge_service::forge_service(vcs_service& vcs, forge_options options)
	: m_vcs(vcs), m_options(std::make_shared<const forge_options>(std::move(options)))
{
}

edt::forge_service::~forge_service() = default;

edt::vcs_policy edt::forge_service::current_policy() const
{
	return m_policy_source ? m_policy_source() : vcs_policy{};
}

void edt::forge_service::remember_shown(std::string key, std::string target)
{
	const auto now = this->now();
	std::erase_if(m_shown, [&](const shown_target& shown) { return shown.key == key || now - shown.at > k_shown_lifetime; });
	if (m_shown.size() >= k_max_shown) m_shown.erase(m_shown.begin());
	m_shown.push_back({ std::move(key), std::move(target), now });
}

std::optional<std::string> edt::forge_service::take_shown(const std::string& key)
{
	const auto now = this->now();
	std::erase_if(m_shown, [&](const shown_target& shown) { return now - shown.at > k_shown_lifetime; });
	const auto found = std::find_if(m_shown.begin(), m_shown.end(), [&](const shown_target& shown) { return shown.key == key; });
	if (found == m_shown.end()) return std::nullopt;
	std::string target = std::move(found->target);
	m_shown.erase(found);
	return target;
}

// `before` + the repository + `after` is the text; `refusal`, when set, is why the call will be refused (the job says the same).
std::optional<std::string> edt::forge_service::describe(std::string before, const std::string& remote, std::string key,
	const cmd::origin& caller, std::string after, std::optional<std::string> refusal)
{
	if (caller.kind == cmd::origin_kind::ui) return std::nullopt;
	std::string target = "refused";
	if (!refusal) {
		const vcs_policy policy = current_policy();
		const vcs_service::remote_snapshot snapshot = m_remote_reader ? m_remote_reader(remote) : m_vcs.read_remote_now(remote);
		forge::forge_remote parsed;
		if (snapshot.code == vcs_service::k_describe_timeout) {
			// "refused" is what the job compares with, so it denies too.
			remember_shown(std::move(key), target);
			return std::string("repository could not be determined, will be refused");
		}
		if (!snapshot.ok()) {
			refusal = snapshot.error;
		} else if (!remote_in_policy(policy, snapshot.remote)) {
			refusal = std::format("the agent policy does not allow the remote '{}' (vcs.remotes)", snapshot.remote);
		} else if (auto failure = judge_url(*m_options, true, snapshot.remote, snapshot.url, parsed)) {
			refusal = failure->message;
		} else {
			target = target_text(parsed, snapshot.remote);
		}
	}
	remember_shown(std::move(key), target);
	const std::string text = refusal ? "This request will be refused: " + *refusal : before + target + after;
	return forge::sanitize_text(text, 400);
}

std::optional<std::string> edt::forge_service::describe_create_pr(const create_request& request, const cmd::origin& caller)
{
	const bool guarded = caller.kind != cmd::origin_kind::ui;
	std::optional<std::string> refusal;
	if (guarded && request.draft_given && !request.draft) {
		refusal = "a pull request made by an agent is always a draft: draft:false is refused";
	} else if (auto error = request_error(request)) {
		refusal = std::move(*error);
	} else if (auto why = vcs_push_branch_refusal(current_policy(), request.head)) {
		refusal = "a pull request's head must be a branch an agent may push: " + *why;
	}
	return describe("Create a draft PR in ", request.remote,
		call_key(caller, "create_pr", { request.remote, request.base, request.head, request.title, request.body }), caller,
		std::format(": {} -> {}", request.head, request.base), std::move(refusal));
}

std::optional<std::string> edt::forge_service::describe_pr_status(std::int64_t number, const std::string& remote, const cmd::origin& caller)
{
	const std::string numeral = std::to_string(number);
	return describe(std::format("Read pull request #{} of ", number), remote, call_key(caller, "pr_status", { remote, numeral }), caller, {},
		number <= 0 ? std::optional<std::string>{ "number must be a pull request number" } : std::nullopt);
}

std::optional<std::string> edt::forge_service::describe_list_prs(const list_request& request, const cmd::origin& caller)
{
	const std::string limit = std::to_string(std::clamp(request.limit, 1, k_max_list_limit));
	return describe("List pull requests of ", request.remote,
		call_key(caller, "list_prs", { request.remote, request.state, request.head, request.base, limit }), caller, {},
		request.state == "open" || request.state == "closed" || request.state == "all"
			? std::nullopt : std::optional<std::string>{ "state must be open, closed or all" });
}

edt::vcs_service::started edt::forge_service::start_create_pr(const create_request& request, const cmd::origin& caller, bool approved)
{
	const bool guarded = caller.kind != cmd::origin_kind::ui;
	// Only a person opens a pull request that is ready for review.
	if (guarded && request.draft_given && !request.draft) {
		return { 0, cmd::codes::denied, "a pull request made by an agent is always a draft: draft:false is refused" };
	}
	if (auto error = request_error(request)) return { 0, cmd::codes::invalid_args, std::move(*error) };

	call_plan plan;
	plan.label = "forge create_pr";
	plan.remote = request.remote;
	plan.guarded = guarded;
	if (guarded) {
		plan.policy = current_policy();
		plan.head = request.head;
		plan.approved = approved;
		plan.shown = take_shown(call_key(caller, "create_pr", { request.remote, request.base, request.head, request.title, request.body }));
	}
	forge::create_pr_request wanted;
	wanted.base = request.base;
	wanted.head = request.head;
	wanted.title = request.title;
	wanted.body = request.body;
	wanted.draft = guarded ? true : (request.draft_given ? request.draft : true);
	return start_call(m_vcs, m_options, std::move(plan), caller, [wanted = std::move(wanted)](forge::forge_provider& provider,
		const target& found, std::stop_token stop) -> json::value {
		const auto created = provider.create_pr(found.remote, wanted, found.token, stop);
		if (!created) return forge_failure(created.error(), found.token);
		return pr_json(created.value(), found.token);
	});
}

edt::vcs_service::started edt::forge_service::start_pr_status(std::int64_t number, const std::string& remote, const cmd::origin& caller, bool approved)
{
	if (number <= 0) return { 0, cmd::codes::invalid_args, "number must be a pull request number" };
	call_plan plan;
	plan.label = "forge pr_status";
	plan.remote = remote;
	plan.guarded = caller.kind != cmd::origin_kind::ui;
	if (plan.guarded) {
		plan.policy = current_policy();
		plan.approved = approved;
		plan.shown = take_shown(call_key(caller, "pr_status", { remote, std::to_string(number) }));
	}
	return start_call(m_vcs, m_options, std::move(plan), caller, [number](forge::forge_provider& provider, const target& found,
		std::stop_token stop) -> json::value {
		const auto status = provider.pr_status(found.remote, number, found.token, stop);
		if (!status) return forge_failure(status.error(), found.token);
		return pr_json(status.value(), found.token);
	});
}

edt::vcs_service::started edt::forge_service::start_list_prs(const list_request& request, const cmd::origin& caller, bool approved)
{
	if (request.state != "open" && request.state != "closed" && request.state != "all") {
		return { 0, cmd::codes::invalid_args, "state must be open, closed or all" };
	}
	call_plan plan;
	plan.label = "forge list_prs";
	plan.remote = request.remote;
	plan.guarded = caller.kind != cmd::origin_kind::ui;
	if (plan.guarded) {
		plan.policy = current_policy();
		plan.approved = approved;
		const std::string limit = std::to_string(std::clamp(request.limit, 1, k_max_list_limit));
		plan.shown = take_shown(call_key(caller, "list_prs", { request.remote, request.state, request.head, request.base, limit }));
	}
	forge::list_prs_filter filter;
	filter.state = request.state;
	if (!request.head.empty()) filter.head = request.head;
	if (!request.base.empty()) filter.base = request.base;
	filter.limit = std::clamp(request.limit, 1, k_max_list_limit);
	return start_call(m_vcs, m_options, std::move(plan), caller, [filter = std::move(filter)](forge::forge_provider& provider,
		const target& found, std::stop_token stop) -> json::value {
		const auto listed = provider.list_prs(found.remote, filter, found.token, stop);
		if (!listed) return forge_failure(listed.error(), found.token);
		json::array prs;
		prs.reserve(listed.value().size());
		for (const forge::pr_info& pr : listed.value()) prs.push_back(pr_json(pr, found.token));
		return json::object{ { "prs", std::move(prs) } };
	});
}

std::vector<edt::registration> edt::register_forge_commands(cmd::command_registry& commands, forge_service& service)
{
	std::vector<registration> out;

	cmd::command_desc create = forge_command("forge.create_pr", "Start opening a pull request on the code host of the project's "
		"git remote; always a draft when an agent asks, and a person confirms it by default; the result arrives in job.get",
		R"({
		"type": "object",
		"properties": {
			"base": { "type": "string", "minLength": 1, "description": "The branch to merge into" },
			"head": { "type": "string", "minLength": 1, "description": "The local branch to merge, already pushed" },
			"title": { "type": "string", "minLength": 1, "description": "At most 256 bytes, no control characters" },
			"body": { "type": "string", "description": "At most 16 KiB" },
			"draft": { "type": "boolean", "description": "An agent's pull request is always a draft: false is refused" },
			"remote": { "type": "string", "minLength": 1, "description": "A configured remote's name; default: the branch's, else the only one, else origin" }
		},
		"required": ["base", "head", "title"],
		"additionalProperties": false
	})", true);
	create.risk = cmd::danger::confirm;
	create.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_create_pr(create_request_of(request.args.as_object()), request.from, !request.confirmation.empty()));
	};
	create.describe = [&service](const cmd::call& request) {
		return service.describe_create_pr(create_request_of(request.args.as_object()), request.from);
	};
	keep(out, commands.add(std::move(create)));

	cmd::command_desc status = forge_command("forge.pr_status", "Start reading one pull request of the project's git remote "
		"(state, draft, branches, link); the result arrives in job.get", R"({
		"type": "object",
		"properties": {
			"number": { "type": "integer", "minimum": 1 },
			"remote": { "type": "string", "minLength": 1, "description": "A configured remote's name" }
		},
		"required": ["number"],
		"additionalProperties": false
	})", false);
	status.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		const std::int64_t number = cmd::json_schema::integer_of(args.at("number")).value_or(0);
		return job_answer(service.start_pr_status(number, string_arg(args, "remote"), request.from, !request.confirmation.empty()));
	};
	status.describe = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		const std::int64_t number = cmd::json_schema::integer_of(args.at("number")).value_or(0);
		return service.describe_pr_status(number, string_arg(args, "remote"), request.from);
	};
	keep(out, commands.add(std::move(status)));

	cmd::command_desc list = forge_command("forge.list_prs", "Start listing the pull requests of the project's git remote, "
		"newest first, optionally by state, head or base branch; the result arrives in job.get", R"({
		"type": "object",
		"properties": {
			"state": { "type": "string", "enum": ["open", "closed", "all"] },
			"head": { "type": "string", "minLength": 1, "description": "A branch, or owner:branch" },
			"base": { "type": "string", "minLength": 1 },
			"limit": { "type": "integer", "minimum": 1, "maximum": 50 },
			"remote": { "type": "string", "minLength": 1, "description": "A configured remote's name" }
		},
		"additionalProperties": false
	})", false);
	list.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_list_prs(list_request_of(request.args.as_object()), request.from,
			!request.confirmation.empty()));
	};
	list.describe = [&service](const cmd::call& request) {
		return service.describe_list_prs(list_request_of(request.args.as_object()), request.from);
	};
	keep(out, commands.add(std::move(list)));
	return out;
}
