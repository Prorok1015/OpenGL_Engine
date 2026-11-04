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
#include <cstdint>
#include <format>
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

	// Everything up to the network: the rules, the remote and its URL, the token. The job's answer when something is wrong;
	// nullopt with `out` filled when the call may go on. Runs on the io lane (the secret store blocks).
	std::optional<json::value> prepare(const edt::forge_options& options, const call_plan& plan,
		const edt::vcs_service::repository_reader& git, ds::task_context& task, target& out)
	{
		const std::stop_token stop = task.stop;
		if (plan.guarded && !plan.head.empty()) {
			if (auto why = edt::vcs_push_branch_refusal(plan.policy, plan.head)) {
				return error_result("denied", "a pull request's head must be a branch an agent may push: " + *why);
			}
		}
		const auto name = git.remote_name(plan.remote, stop);
		if (!name.ok()) return error_result(name.code, name.error);
		if (plan.guarded && std::find(plan.policy.remotes.begin(), plan.policy.remotes.end(), name.text) == plan.policy.remotes.end()) {
			return error_result("denied", std::format("the agent policy does not allow the remote '{}' (vcs.remotes)", name.text));
		}
		const auto url = git.remote_url(name.text, stop);
		if (!url.ok()) return error_result(url.code, url.error);
		auto parsed = edt::forge::parse_forge_remote(url.text);
		if (!parsed) return error_result(edt::forge::forge_error_code_name(parsed.error().code), parsed.error().message);
		if (!host_allowed(options, parsed.value().host)) {
			return error_result(plan.guarded ? "denied" : "host_not_allowed",
				std::format("the host '{}' of the remote '{}' is not one pull requests are made on", parsed.value().host, name.text));
		}
		if (plan.guarded && !plan.head.empty()) {
			const auto branch = git.local_branch(plan.head, stop);
			if (!branch.ok()) return error_result(branch.code, branch.error);
		}

		const platform::secret_key key{ edt::forge_service::k_secret_service, parsed.value().host };
		const platform::secret_result loaded = options.secrets ? options.secrets->load(key, out.token)
			: platform::secret_result{ platform::secret_status::unavailable, "no secret store" };
		if (stop.stop_requested()) throw ds::task_cancelled{};
		if (loaded.status == platform::secret_status::not_found || (loaded.status == platform::secret_status::ok && out.token.empty())) {
			return error_result("no_token", std::format("no token is stored for {}: add one in Settings (Version Control)", parsed.value().host));
		}
		if (loaded.status != platform::secret_status::ok) {
			return error_result(edt::forge::forge_error_code_name(edt::forge::forge_error_code::unavailable),
				"the secret store could not be read: " + loaded.message);
		}
		egLOG_DEBUG("forge", "{} on {}", plan.label, parsed.value().host);
		out.remote = std::move(parsed.value());
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
		// Nothing a title can hide behind: control, bidi and invisible characters would change under sanitize_text.
		if (edt::forge::sanitize_text(request.title, request.title.size() + 4) != request.title) {
			return std::string("title must be valid UTF-8 without control, bidirectional or invisible characters");
		}
		if (request.body.size() > edt::forge_service::k_max_body) {
			return std::format("body must be at most {} bytes", edt::forge_service::k_max_body);
		}
		if (!edt::forge::is_valid_utf8(request.body) || request.body.find('\0') != std::string::npos) {
			return std::string("body must be valid UTF-8 without NUL");
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

edt::vcs_service::started edt::forge_service::start_create_pr(const create_request& request, const cmd::origin& caller)
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

edt::vcs_service::started edt::forge_service::start_pr_status(std::int64_t number, const std::string& remote, const cmd::origin& caller)
{
	if (number <= 0) return { 0, cmd::codes::invalid_args, "number must be a pull request number" };
	call_plan plan;
	plan.label = "forge pr_status";
	plan.remote = remote;
	plan.guarded = caller.kind != cmd::origin_kind::ui;
	if (plan.guarded) plan.policy = current_policy();
	return start_call(m_vcs, m_options, std::move(plan), caller, [number](forge::forge_provider& provider, const target& found,
		std::stop_token stop) -> json::value {
		const auto status = provider.pr_status(found.remote, number, found.token, stop);
		if (!status) return forge_failure(status.error(), found.token);
		return pr_json(status.value(), found.token);
	});
}

edt::vcs_service::started edt::forge_service::start_list_prs(const list_request& request, const cmd::origin& caller)
{
	if (request.state != "open" && request.state != "closed" && request.state != "all") {
		return { 0, cmd::codes::invalid_args, "state must be open, closed or all" };
	}
	call_plan plan;
	plan.label = "forge list_prs";
	plan.remote = request.remote;
	plan.guarded = caller.kind != cmd::origin_kind::ui;
	if (plan.guarded) plan.policy = current_policy();
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
		const json::object& args = request.args.as_object();
		forge_service::create_request wanted;
		wanted.base = string_arg(args, "base");
		wanted.head = string_arg(args, "head");
		wanted.title = string_arg(args, "title");
		wanted.body = string_arg(args, "body");
		wanted.remote = string_arg(args, "remote");
		if (const json::value* draft = args.if_contains("draft")) {
			wanted.draft = draft->as_bool();
			wanted.draft_given = true;
		}
		return job_answer(service.start_create_pr(wanted, request.from));
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
		return job_answer(service.start_pr_status(number, string_arg(args, "remote"), request.from));
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
		const json::object& args = request.args.as_object();
		forge_service::list_request wanted;
		if (const json::value* state = args.if_contains("state")) wanted.state = std::string(state->as_string());
		wanted.head = string_arg(args, "head");
		wanted.base = string_arg(args, "base");
		wanted.remote = string_arg(args, "remote");
		if (const json::value* limit = args.if_contains("limit")) {
			wanted.limit = static_cast<int>(cmd::json_schema::integer_of(*limit).value_or(forge_service::k_default_list_limit));
		}
		return job_answer(service.start_list_prs(wanted, request.from));
	};
	keep(out, commands.add(std::move(list)));
	return out;
}
