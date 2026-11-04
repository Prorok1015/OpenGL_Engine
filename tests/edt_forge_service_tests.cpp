#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include "edt_forge_service.h"
#include "edt_github_provider.h"
#include "edt_vcs_service.h"
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_types.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/platform/plt_process.h>
#include <snk/platform/plt_secret.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// forge_service (#458, PR-3) on a manual task system: a fake git for the remote, a fake HTTP transport with recorded GitHub
// answers and a secret store in RAM. Nothing here touches git, the network or the OS store.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace std::string_literals;

const std::string canary = "ghp_CANARYCANARYCANARYCANARY0123456789";

bool has_arg(const std::vector<std::string>& args, std::string_view arg)
{
	return std::find(args.begin(), args.end(), arg) != args.end();
}

platform::process_result exited(int code)
{
	platform::process_result result;
	result.exit_code = code;
	return result;
}

bool has_canary(std::string_view text)
{
	return text.find(canary) != std::string_view::npos;
}

std::string text_of(const json::value& value)
{
	return std::string(value.as_string());
}

const std::string pr_created = R"json({
	"number": 42, "title": "Add the forge layer", "state": "open", "draft": true, "merged": false,
	"html_url": "https://github.com/octo/engine/pull/42",
	"head": { "ref": "agent/forge", "sha": "abc" }, "base": { "ref": "main", "sha": "def" },
	"user": { "login": "octo" }
})json";

struct forge_fixture
{
	forge_fixture()
	{
		ds::task_system_config config;
		config.mode = ds::policy::manual;
		tasks = std::make_unique<ds::task_system>(config);
		jobs = std::make_unique<edt::job_service>(*tasks, &events);
		project = fs::temp_directory_path() / "snk_edt_forge_tests_project";
		secrets = std::make_shared<platform::secret_store>(std::make_unique<platform::memory_secret_backend>());
		store_token();
		remote_urls = { { "origin", "https://github.com/octo/engine.git" } };
		branches = { "agent/forge", "feature/x", "main" };
		make_services(true);
	}

	~forge_fixture()
	{
		registrations.clear();
		forge.reset();
		vcs.reset();
		jobs->cancel_all_and_wait();
	}

	void store_token()
	{
		const platform::secret_result stored = secrets->store({ "forge", "github.com" }, platform::secret_text{ std::string(canary) });
		BOOST_REQUIRE((stored.status == platform::secret_status::ok));
	}

	void make_services(bool with_project)
	{
		registrations.clear();
		forge.reset();
		vcs.reset();
		edt::vcs_options options;
		options.git = "fake-git";
		options.project_root = [this, with_project] { return with_project ? std::optional<fs::path>{ project } : std::nullopt; };
		options.environment = std::vector<std::string>{ "PATH=/nowhere" };
		options.run = [this](const fs::path&, const std::vector<std::string>& args, const platform::process_options& opts,
			const std::function<void(std::string_view)>& on_chunk, std::stop_token) -> platform::process_result {
			return fake_git(args, opts, on_chunk);
		};
		vcs = std::make_unique<edt::vcs_service>(*jobs, std::move(options));
		edt::forge_options forge_setup;
		forge_setup.provider = std::make_shared<edt::forge::github_provider>(
			[this](const edt::forge::http_request& request, std::stop_token stop) { return answer_http(request, stop); });
		forge_setup.secrets = secrets;
		forge = std::make_unique<edt::forge_service>(*vcs, std::move(forge_setup));
		forge->set_policy_source([this] { return policy; });
		registrations = edt::register_forge_commands(commands, *forge);
	}

	platform::process_result fake_git(const std::vector<std::string>& args, const platform::process_options& opts,
		const std::function<void(std::string_view)>& on_chunk)
	{
		git_calls.push_back(args);
		const auto fail = [&opts](std::string_view line, int code) {
			if (opts.on_stderr_line) opts.on_stderr_line(line);
			return exited(code);
		};
		if (has_arg(args, "--version")) {
			on_chunk("git version 2.45.1\n");
			return exited(0);
		}
		if (has_arg(args, "--get-all")) {
			// The URL as written in the config, with NUL as the end of a value (config -z).
			const std::string key = args.back();
			if (key.ends_with(".pushurl")) {
				for (const auto& [remote, url] : push_urls) {
					if (key == "remote." + remote + ".pushurl") {
						on_chunk(url + std::string(1, '\0'));
						return exited(0);
					}
				}
				return exited(1);
			}
			for (const auto& [remote, url] : remote_urls) {
				if (key == "remote." + remote + ".url") {
					on_chunk(url + std::string(1, '\0'));
					return exited(0);
				}
			}
			return exited(1);
		}
		if (has_arg(args, "--get-regexp")) {
			const bool asks_for_rewrites = std::any_of(args.begin(), args.end(),
				[](const std::string& arg) { return arg.find("insteadof") != std::string::npos; });
			if (!asks_for_rewrites || rewrites.empty()) return exited(1);
			for (const auto& [key, value] : rewrites) on_chunk(key + "\n" + value + std::string(1, '\0'));
			return exited(0);
		}
		if (has_arg(args, "rev-parse")) {
			if (has_arg(args, "--verify")) {
				const std::string wanted = args.back();
				for (const std::string& branch : branches) {
					if (wanted == "refs/heads/" + branch + "^{commit}") {
						on_chunk("0123456789abcdef0123456789abcdef01234567\n");
						return exited(0);
					}
				}
				return exited(1);
			}
			if (not_a_repository) return fail("fatal: not a git repository (or any of the parent directories): .git", 128);
			on_chunk(project.generic_string() + "\n");
			return exited(0);
		}
		if (has_arg(args, "symbolic-ref")) {
			if (detached) return exited(1);
			on_chunk(current_branch + "\n");
			return exited(0);
		}
		if (has_arg(args, "config") && has_arg(args, "--get")) {
			if (branch_remote.empty()) return exited(1);
			on_chunk(branch_remote + "\n");
			return exited(0);
		}
		if (has_arg(args, "get-url")) {
			const std::string name = args.back();
			for (const auto& [remote, url] : remote_urls) {
				if (remote == name) {
					on_chunk(url + "\n");
					return exited(0);
				}
			}
			return fail("error: No such remote '" + name + "'", 2);
		}
		if (has_arg(args, "remote")) {
			for (const auto& [remote, url] : remote_urls) on_chunk(remote + "\n");
			return exited(0);
		}
		return fail("fatal: the fake git does not know this call", 129);
	}

	edt::forge::http_response answer_http(const edt::forge::http_request& request, std::stop_token stop)
	{
		requests.push_back(request);
		tokens_seen.push_back(request.bearer_token != nullptr ? request.bearer_token->reveal() : std::string{});
		if (on_http) on_http(stop);
		edt::forge::http_response response;
		response.status = next_status;
		response.body = next_body;
		response.error_text = next_error;
		return response;
	}

	void answer(int status, std::string body)
	{
		next_status = status;
		next_body = std::move(body);
		next_error.clear();
	}

	void run_to_end()
	{
		while (tasks->run_one(ds::lane::io)) {}
		tasks->drain_main();
	}

	json::value finish(edt::job_service::job_id id)
	{
		run_to_end();
		const auto info = jobs->info(id);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done, "the job did not end done: " + info->error);
		BOOST_REQUIRE(info->result.has_value());
		return *info->result;
	}

	cmd::result invoke(std::string_view id, const std::string& args, cmd::origin from)
	{
		return commands.invoke(id, json::parse(args), std::move(from));
	}

	// The job's result of a command that must start one.
	json::value run(std::string_view id, const std::string& args, cmd::origin from = cmd::origin::agent("0123abcd"))
	{
		const cmd::result started = invoke(id, args, std::move(from));
		BOOST_REQUIRE_MESSAGE(started.ok, started.code + ": " + started.error);
		return finish(static_cast<edt::job_service::job_id>(started.value.as_object().at("job").as_int64()));
	}

	static bool is_error(const json::value& result, std::string_view code)
	{
		const json::object& object = result.as_object();
		return object.contains("error") && object.at("error").as_object().at("code").as_string() == code;
	}

	static std::string create_args(std::string_view head = "agent/forge", std::string_view extra = {})
	{
		return R"({"base":"main","head":")" + std::string(head) + R"(","title":"Add the forge layer","body":"Details")"
			+ std::string(extra) + "}";
	}

	bool git_asked(std::string_view word) const
	{
		return std::any_of(git_calls.begin(), git_calls.end(), [word](const std::vector<std::string>& args) { return has_arg(args, word); });
	}

	json::value request_body(std::size_t index = 0) const
	{
		BOOST_REQUIRE(index < requests.size());
		return json::parse(requests[index].body);
	}

	edt::editor_events events;
	std::unique_ptr<ds::task_system> tasks;
	std::unique_ptr<edt::job_service> jobs;
	fs::path project;
	std::shared_ptr<platform::secret_store> secrets;
	std::unique_ptr<edt::vcs_service> vcs;
	std::unique_ptr<edt::forge_service> forge;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
	edt::vcs_policy policy;

	std::vector<std::pair<std::string, std::string>> remote_urls;
	// remote -> pushurl, and url.<base>.insteadof -> the prefix it replaces.
	std::vector<std::pair<std::string, std::string>> push_urls;
	std::vector<std::pair<std::string, std::string>> rewrites;
	std::vector<std::string> branches;
	std::string current_branch = "agent/forge";
	std::string branch_remote;
	bool detached = false;
	bool not_a_repository = false;
	std::vector<std::vector<std::string>> git_calls;

	int next_status = 201;
	std::string next_body = pr_created;
	std::string next_error;
	std::vector<edt::forge::http_request> requests;
	std::vector<std::string> tokens_seen;
	std::function<void(std::stop_token)> on_http;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtForgeService, forge_fixture)

BOOST_AUTO_TEST_CASE(CreatePr_FromAnAgent_ReturnsThePullRequestAsADraft)
{
	const json::value result = run("forge.create_pr", create_args());

	BOOST_TEST(!result.as_object().contains("error"));
	BOOST_TEST(result.as_object().at("number").as_int64() == 42);
	BOOST_TEST(text_of(result.as_object().at("title")) == "Add the forge layer");
	BOOST_TEST(text_of(result.as_object().at("state")) == "open");
	BOOST_TEST(result.as_object().at("draft").as_bool());
	BOOST_TEST(text_of(result.as_object().at("head")) == "agent/forge");
	BOOST_TEST(text_of(result.as_object().at("base")) == "main");
	BOOST_TEST(text_of(result.as_object().at("url")) == "https://github.com/octo/engine/pull/42");
	BOOST_TEST(result.as_object().size() == 7u);

	BOOST_REQUIRE(requests.size() == 1u);
	BOOST_TEST(requests[0].method == "POST");
	BOOST_TEST(requests[0].url == "https://api.github.com/repos/octo/engine/pulls");
	BOOST_TEST(request_body().as_object().at("draft").as_bool());
	BOOST_TEST(tokens_seen[0] == canary);
}

BOOST_AUTO_TEST_CASE(CreatePr_AnAgentCannotAskForAReadyPullRequest_NoJobNoRequest)
{
	const cmd::result refused = invoke("forge.create_pr", create_args("agent/forge", R"(,"draft":false)"), cmd::origin::agent("0123abcd"));

	BOOST_TEST(!refused.ok);
	BOOST_TEST(refused.code == cmd::codes::denied);
	BOOST_TEST(requests.empty());
	BOOST_TEST(!git_asked("get-url"));
	// The other origins that are not a person's are held to it as well.
	BOOST_TEST(invoke("forge.create_pr", create_args("agent/forge", R"(,"draft":false)"), cmd::origin::test()).code == cmd::codes::denied);
	BOOST_TEST(invoke("forge.create_pr", create_args("agent/forge", R"(,"draft":false)"), cmd::origin::game()).code == cmd::codes::denied);
	BOOST_TEST(requests.empty());
	// An explicit true is fine.
	const json::value fine = run("forge.create_pr", create_args("agent/forge", R"(,"draft":true)"));
	BOOST_TEST(fine.as_object().at("number").as_int64() == 42);
}

BOOST_AUTO_TEST_CASE(CreatePr_APersonMayOpenAReadyPullRequest_AndGetsADraftWhenSilent)
{
	next_body = R"json({"number": 7, "title": "t", "state": "open", "draft": false, "html_url": "https://github.com/octo/engine/pull/7",
		"head": { "ref": "feature/x" }, "base": { "ref": "main" }})json";
	const json::value ready = run("forge.create_pr", create_args("feature/x", R"(,"draft":false)"), cmd::origin::ui());

	BOOST_TEST(!ready.as_object().contains("error"));
	BOOST_REQUIRE(requests.size() == 1u);
	BOOST_TEST(!request_body().as_object().at("draft").as_bool());

	run("forge.create_pr", create_args("feature/x"), cmd::origin::ui());
	BOOST_REQUIRE(requests.size() == 2u);
	BOOST_TEST(request_body(1).as_object().at("draft").as_bool());
}

BOOST_AUTO_TEST_CASE(CreatePr_TheHeadMustPassThePushRules_BeforeAnyRequest)
{
	// Not in push_branches.
	BOOST_TEST(is_error(run("forge.create_pr", create_args("feature/x")), "denied"));
	// Protected, even when push_branches would take it.
	policy.push_branches = { "*" };
	BOOST_TEST(is_error(run("forge.create_pr", create_args("main")), "denied"));
	BOOST_TEST(is_error(run("forge.create_pr", create_args("Master")), "denied"));
	branches.push_back("Master");
	BOOST_TEST(is_error(run("forge.create_pr", create_args("Master")), "denied"));
	BOOST_TEST(requests.empty());
	BOOST_TEST(!git_asked("get-url"));

	// A policy that lists the branch lets it through.
	policy.push_branches = { "feature/*" };
	const json::value allowed = run("forge.create_pr", create_args("feature/x"));
	BOOST_TEST(!allowed.as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);

	// A person is not held to the push rules.
	policy.push_branches = { "agent/*" };
	requests.clear();
	BOOST_TEST(!run("forge.create_pr", create_args("feature/x"), cmd::origin::ui()).as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);
}

BOOST_AUTO_TEST_CASE(CreatePr_TheHeadMustExistLocally_ForAnAgent)
{
	const json::value missing = run("forge.create_pr", create_args("agent/not-made-yet"));

	BOOST_TEST(is_error(missing, "no_local_branch"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(CreatePr_ArgumentsAreCheckedForEveryone)
{
	for (const cmd::origin& from : { cmd::origin::ui(), cmd::origin::agent("0123abcd") }) {
		const auto invalid = [&](const std::string& args) { return invoke("forge.create_pr", args, from).code == cmd::codes::invalid_args; };
		BOOST_TEST(invalid(R"({"base":"main","head":"a..b","title":"t"})"));
		BOOST_TEST(invalid(R"({"base":"-main","head":"agent/x","title":"t"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"line\u0007bell"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"bidi\u202etitle"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"invisible\u200btitle"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":""})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u0000b"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u0007b"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u001bb"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u007fb"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u202eb"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u200bb"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t","body":"a\u2028b"})"));
		BOOST_TEST(invalid(R"({"base":"main","head":"agent/x","title":"t\u202e"})"));

		json::object long_title{ { "base", "main" }, { "head", "agent/x" }, { "title", json::string(std::string(257, 't')) } };
		BOOST_TEST((commands.invoke("forge.create_pr", long_title, from).code == cmd::codes::invalid_args));
		json::object long_body{ { "base", "main" }, { "head", "agent/x" }, { "title", "t" },
			{ "body", json::string(std::string(16 * 1024 + 1, 'b')) } };
		BOOST_TEST((commands.invoke("forge.create_pr", long_body, from).code == cmd::codes::invalid_args));
		json::object edge{ { "base", "main" }, { "head", "agent/forge" }, { "title", json::string(std::string(256, 't')) },
			{ "body", json::string(std::string(16 * 1024, 'b')) } };
		BOOST_TEST(commands.invoke("forge.create_pr", edge, from).ok);
	}
	run_to_end();
}

BOOST_AUTO_TEST_CASE(Body_MayHaveTabsAndLineBreaks)
{
	const json::value result = run("forge.create_pr", R"({"base":"main","head":"agent/forge","title":"t","body":"one\ttwo\r\nthree\n\u00e9"})");

	BOOST_TEST(!result.as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);
}

BOOST_AUTO_TEST_CASE(OwnerAndRepository_NeverComeFromArguments)
{
	for (const char* extra : { R"(,"owner":"evil")", R"(,"repo":"other")", R"(,"url":"https://github.com/evil/x")",
			R"(,"host":"evil.example")", R"(,"token":"abc")" }) {
		BOOST_TEST(invoke("forge.create_pr", create_args("agent/forge", extra), cmd::origin::ui()).code == cmd::codes::invalid_args);
	}
	BOOST_TEST(invoke("forge.pr_status", R"({"number":1,"owner":"evil"})", cmd::origin::ui()).code == cmd::codes::invalid_args);
	BOOST_TEST(invoke("forge.list_prs", R"({"repo":"evil/x"})", cmd::origin::ui()).code == cmd::codes::invalid_args);
	BOOST_TEST(invoke("forge.list_prs", R"({"limit":51})", cmd::origin::ui()).code == cmd::codes::invalid_args);
	BOOST_TEST(invoke("forge.list_prs", R"({"limit":0})", cmd::origin::ui()).code == cmd::codes::invalid_args);
	BOOST_TEST(invoke("forge.list_prs", R"({"state":"merged"})", cmd::origin::ui()).code == cmd::codes::invalid_args);
	BOOST_TEST(invoke("forge.pr_status", R"({"number":0})", cmd::origin::ui()).code == cmd::codes::invalid_args);
	BOOST_TEST(requests.empty());

	// The repository in the request is the git remote's, whatever the title says.
	remote_urls = { { "origin", "git@github.com:Real-Owner/real.repo.git" } };
	const json::value result = run("forge.create_pr", R"({"base":"main","head":"agent/forge","title":"octo/engine evil/other"})");
	BOOST_TEST(!result.as_object().contains("error"));
	BOOST_REQUIRE(requests.size() == 1u);
	BOOST_TEST(requests[0].url == "https://api.github.com/repos/Real-Owner/real.repo/pulls");
}

BOOST_AUTO_TEST_CASE(Remote_MustBeInThePolicy_ForAnAgent)
{
	remote_urls = { { "upstream", "https://github.com/octo/engine.git" } };

	const json::value implicit = run("forge.create_pr", create_args());
	BOOST_TEST(is_error(implicit, "denied"));
	const json::value named = run("forge.pr_status", R"({"number":1,"remote":"upstream"})");
	BOOST_TEST(is_error(named, "denied"));
	BOOST_TEST(is_error(run("forge.list_prs", "{}"), "denied"));
	BOOST_TEST(requests.empty());
	BOOST_TEST(!git_asked("get-url"));

	policy.remotes = { "upstream" };
	BOOST_TEST(!run("forge.create_pr", create_args()).as_object().contains("error"));
}

namespace {

edt::forge_service::create_request create_request_for_the_tests()
{
	edt::forge_service::create_request request;
	request.base = "main";
	request.head = "agent/forge";
	request.title = "Add the forge layer";
	request.body = "Details";
	return request;
}

}

BOOST_AUTO_TEST_CASE(Describe_NamesTheRepositoryTheRequestWouldGoTo_ForAnAgent)
{
	const std::optional<std::string> text = forge->describe_create_pr(create_request_for_the_tests(), cmd::origin::agent("0123abcd"));

	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(text->find("github.com/octo/engine") != std::string::npos);
	BOOST_TEST(text->find("origin") != std::string::npos);
	BOOST_TEST(text->find("agent/forge -> main") != std::string::npos);
	BOOST_TEST(text->find("draft") != std::string::npos);
	const std::optional<std::string> reading = forge->describe_pr_status(3, "", cmd::origin::agent("0123abcd"));
	BOOST_REQUIRE(reading.has_value());
	BOOST_TEST(reading->find("#3") != std::string::npos);
	BOOST_TEST(reading->find("github.com/octo/engine") != std::string::npos);
	const std::optional<std::string> listing = forge->describe_list_prs(edt::forge_service::list_request{}, cmd::origin::agent("0123abcd"));
	BOOST_REQUIRE(listing.has_value());
	BOOST_TEST(listing->find("github.com/octo/engine") != std::string::npos);
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(Describe_IsNothingForAPerson)
{
	BOOST_TEST(!forge->describe_create_pr(create_request_for_the_tests(), cmd::origin::ui()).has_value());
	BOOST_TEST(!forge->describe_pr_status(3, "", cmd::origin::ui()).has_value());
	BOOST_TEST(!forge->describe_list_prs(edt::forge_service::list_request{}, cmd::origin::ui()).has_value());
}

BOOST_AUTO_TEST_CASE(Describe_ReadsTheUrlAsWritten_NotWhatGitReportsAfterRewrites)
{
	remote_urls = { { "origin", "https://github.com/octo/engine.git" } };
	git_calls.clear();

	forge->describe_create_pr(create_request_for_the_tests(), cmd::origin::agent("0123abcd"));

	BOOST_TEST(!git_asked("get-url"));
	BOOST_TEST(git_asked("--get-all"));
}

BOOST_AUTO_TEST_CASE(Describe_ARefusedRequest_SaysSo)
{
	edt::forge_service::create_request ready = create_request_for_the_tests();
	ready.draft = false;
	ready.draft_given = true;
	const std::optional<std::string> text = forge->describe_create_pr(ready, cmd::origin::agent("0123abcd"));

	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(text->starts_with("This request will be refused"));
}

BOOST_AUTO_TEST_CASE(UrlRewrite_ThatAppliesToTheRemote_IsRefusedWithNoRequest)
{
	// What `git remote get-url` would report is github.com/attacker/x; the config as written says octo/engine.
	rewrites = { { "url.https://github.com/attacker/x.insteadof", "https://github.com/octo/engine" } };

	const std::optional<std::string> text = forge->describe_create_pr(create_request_for_the_tests(), cmd::origin::agent("0123abcd"));
	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(text->starts_with("This request will be refused"));
	BOOST_TEST(text->find("rewrites") != std::string::npos);
	BOOST_TEST(text->find("global") != std::string::npos);

	BOOST_TEST(is_error(run("forge.create_pr", create_args()), "denied"));
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "denied"));
	BOOST_TEST(is_error(run("forge.list_prs", "{}"), "denied"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(UrlRewrite_ThatDoesNotApplyToTheRemote_IsNoObstacle)
{
	rewrites = { { "url.https://github.com/attacker/x.insteadof", "https://elsewhere.example/" },
		{ "url.https://github.com/attacker/y.pushinsteadof", "git@gitlab.example:" } };

	BOOST_TEST(!run("forge.create_pr", create_args()).as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);
}

BOOST_AUTO_TEST_CASE(UrlRewrite_AnEmptyPrefixMatchesEveryUrl)
{
	rewrites = { { "url.https://github.com/attacker/x.insteadof", "" } };

	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "denied"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(UrlRewrite_DoesNotStopAPerson)
{
	rewrites = { { "url.https://github.com/attacker/x.insteadof", "https://github.com/octo/engine" } };

	BOOST_TEST(!run("forge.pr_status", R"({"number":1})", cmd::origin::ui()).as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);
}

BOOST_AUTO_TEST_CASE(PushUrl_OnTheRemote_IsRefusedWithNoRequest)
{
	push_urls = { { "origin", "https://github.com/attacker/x.git" } };

	const std::optional<std::string> text = forge->describe_create_pr(create_request_for_the_tests(), cmd::origin::agent("0123abcd"));
	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(text->find("pushurl") != std::string::npos);
	BOOST_TEST(is_error(run("forge.create_pr", create_args()), "denied"));
	BOOST_TEST(is_error(run("forge.list_prs", "{}"), "denied"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(TheRepositoryShown_IsTheOneTheJobUses)
{
	const cmd::origin agent = cmd::origin::agent("0123abcd");
	BOOST_REQUIRE(forge->describe_create_pr(create_request_for_the_tests(), agent).has_value());

	const json::value result = run("forge.create_pr", create_args());

	BOOST_TEST(!result.as_object().contains("error"));
	BOOST_REQUIRE(requests.size() == 1u);
	BOOST_TEST(requests[0].url == "https://api.github.com/repos/octo/engine/pulls");
}

BOOST_AUTO_TEST_CASE(TheRepositoryChangedAfterTheQuestion_IsDenied_WithNoRequest)
{
	const cmd::origin agent = cmd::origin::agent("0123abcd");
	BOOST_REQUIRE(forge->describe_create_pr(create_request_for_the_tests(), agent).has_value());
	remote_urls = { { "origin", "https://github.com/attacker/x.git" } };

	const json::value result = run("forge.create_pr", create_args());

	BOOST_TEST(is_error(result, "denied"));
	BOOST_TEST(text_of(result.as_object().at("error").as_object().at("message")).find("attacker/x") != std::string::npos);
	BOOST_TEST(requests.empty());

	// What was shown is spent by the job: a later identical call is not held to it.
	BOOST_TEST(!run("forge.create_pr", create_args()).as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);
}

BOOST_AUTO_TEST_CASE(TheRepositoryShown_IsForThatSessionAndThoseArgumentsOnly)
{
	BOOST_REQUIRE(forge->describe_pr_status(1, "", cmd::origin::agent("0123abcd")).has_value());
	remote_urls = { { "origin", "https://github.com/attacker/x.git" } };

	// Another session, and other arguments, were shown nothing: they are not compared.
	BOOST_TEST(!run("forge.pr_status", R"({"number":1})", cmd::origin::agent("ffff0000")).as_object().contains("error"));
	BOOST_TEST(!run("forge.pr_status", R"({"number":2})").as_object().contains("error"));
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "denied"));
}

BOOST_AUTO_TEST_CASE(AnApprovedCall_WhoseShownRepositoryWasEvicted_IsDenied_WithNoRequest)
{
	const cmd::origin agent = cmd::origin::agent("0123abcd");
	BOOST_REQUIRE(forge->describe_create_pr(create_request_for_the_tests(), agent).has_value());
	for (std::int64_t number = 1; number <= 40; ++number) BOOST_REQUIRE(forge->describe_pr_status(number, "", agent).has_value());

	const edt::vcs_service::started started = forge->start_create_pr(create_request_for_the_tests(), agent, true);
	BOOST_REQUIRE(started.code.empty());
	const json::value result = finish(started.job);

	BOOST_TEST(is_error(result, "denied"));
	BOOST_TEST(text_of(result.as_object().at("error").as_object().at("message")).find("no longer known") != std::string::npos);
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(AnApprovedCall_WhoseShownRepositoryExpired_IsDenied_WithNoRequest)
{
	auto now = std::chrono::steady_clock::now();
	forge->set_clock([&now] { return now; });
	const cmd::origin agent = cmd::origin::agent("0123abcd");
	BOOST_REQUIRE(forge->describe_create_pr(create_request_for_the_tests(), agent).has_value());
	now += std::chrono::minutes(6);

	const edt::vcs_service::started started = forge->start_create_pr(create_request_for_the_tests(), agent, true);
	BOOST_REQUIRE(started.code.empty());

	BOOST_TEST(is_error(finish(started.job), "denied"));
	BOOST_TEST(requests.empty());
	forge->set_clock({});
}

BOOST_AUTO_TEST_CASE(AnApprovedCall_WithTheShownRepository_Runs_AndACallNobodyWasAskedAbout_Runs)
{
	const cmd::origin agent = cmd::origin::agent("0123abcd");
	BOOST_REQUIRE(forge->describe_create_pr(create_request_for_the_tests(), agent).has_value());
	const edt::vcs_service::started approved = forge->start_create_pr(create_request_for_the_tests(), agent, true);
	BOOST_REQUIRE(approved.code.empty());
	BOOST_TEST(!finish(approved.job).as_object().contains("error"));

	const edt::vcs_service::started unasked = forge->start_create_pr(create_request_for_the_tests(), agent, false);
	BOOST_REQUIRE(unasked.code.empty());
	BOOST_TEST(!finish(unasked.job).as_object().contains("error"));
	BOOST_TEST(requests.size() == 2u);
}

BOOST_AUTO_TEST_CASE(Describe_WhenTheRemoteCouldNotBeReadInTime_SaysSo_AndTheApprovedCallIsDenied)
{
	forge->set_remote_reader([](const std::string&) {
		edt::vcs_service::remote_snapshot snapshot;
		snapshot.code = edt::vcs_service::k_describe_timeout;
		snapshot.error = "git did not answer in time";
		return snapshot;
	});
	const cmd::origin agent = cmd::origin::agent("0123abcd");

	const std::optional<std::string> text = forge->describe_create_pr(create_request_for_the_tests(), agent);

	BOOST_REQUIRE(text.has_value());
	BOOST_TEST((*text == "repository could not be determined, will be refused"));
	const edt::vcs_service::started started = forge->start_create_pr(create_request_for_the_tests(), agent, true);
	BOOST_REQUIRE(started.code.empty());
	BOOST_TEST(is_error(finish(started.job), "denied"));
	BOOST_TEST(requests.empty());
	forge->set_remote_reader({});
}

BOOST_AUTO_TEST_CASE(Describe_ThroughTheRegistry_IsBoundToTheTicket)
{
	struct recording_gate : cmd::policy_gate
	{
		cmd::policy_decision decide(const cmd::command_desc&, const cmd::call&) override { return cmd::policy_decision::ask("why"); }
		bool can_ask() const override { return true; }
	};
	commands.set_policy_gate(std::make_shared<recording_gate>());

	const cmd::result asked = invoke("forge.create_pr", create_args(), cmd::origin::agent("0123abcd"));

	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	const auto bound = commands.confirmation_of(std::string(asked.value.as_object().at("ticket").as_string()));
	BOOST_REQUIRE(bound.has_value());
	BOOST_TEST(bound->detail.find("github.com/octo/engine") != std::string::npos);
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(Remote_APersonMayUseAnyConfiguredRemote)
{
	remote_urls = { { "upstream", "https://github.com/octo/engine.git" } };

	const json::value result = run("forge.create_pr", create_args(), cmd::origin::ui());

	BOOST_TEST(!result.as_object().contains("error"));
	BOOST_TEST(requests.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Remote_TheBranchsRemoteWinsOverTheOnlyOne)
{
	remote_urls = { { "origin", "https://github.com/octo/engine.git" }, { "fork", "https://github.com/mine/engine.git" } };
	branch_remote = "origin";
	run("forge.pr_status", R"({"number":3})", cmd::origin::ui());
	BOOST_REQUIRE(requests.size() == 1u);
	BOOST_TEST(requests[0].url == "https://api.github.com/repos/octo/engine/pulls/3");
	run("forge.pr_status", R"({"number":3,"remote":"fork"})", cmd::origin::ui());
	BOOST_REQUIRE(requests.size() == 2u);
	BOOST_TEST(requests[1].url == "https://api.github.com/repos/mine/engine/pulls/3");
}

BOOST_AUTO_TEST_CASE(RemoteUrl_ItsHostMustBeAllowed)
{
	remote_urls = { { "origin", "https://evil.example/octo/engine.git" } };

	BOOST_TEST(is_error(run("forge.create_pr", create_args()), "denied"));
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "denied"));
	BOOST_TEST(is_error(run("forge.create_pr", create_args(), cmd::origin::ui()), "host_not_allowed"));
	BOOST_TEST(requests.empty());

	// The API host is not the host of a remote.
	remote_urls = { { "origin", "https://api.github.com/octo/engine.git" } };
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "denied"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(RemoteUrl_ThatIsNotARepositoryOnAForge_IsAValidationError)
{
	for (const char* url : { "file:///srv/git/engine.git", "https://user:pw@github.com/octo/engine.git", "https://github.com/octo/engine/extra",
			"../engine", "https://github.com/octo/..%2Fx" }) {
		remote_urls = { { "origin", url } };
		BOOST_TEST_CONTEXT(url) {
			BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "validation"));
		}
	}
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(NoToken_IsTheAnswer_AndNothingIsSent)
{
	BOOST_REQUIRE((secrets->erase({ "forge", "github.com" }).status == platform::secret_status::ok));

	const json::value result = run("forge.create_pr", create_args());

	BOOST_TEST(is_error(result, "no_token"));
	BOOST_TEST(text_of(result.as_object().at("error").as_object().at("message")).find("Settings") != std::string::npos);
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "no_token"));
	BOOST_TEST(is_error(run("forge.list_prs", "{}"), "no_token"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(TheTokenIsLookedUpPerHost)
{
	remote_urls = { { "origin", "https://github.com/octo/engine.git" } };
	BOOST_REQUIRE((secrets->erase({ "forge", "github.com" }).status == platform::secret_status::ok));
	BOOST_REQUIRE((secrets->store({ "forge", "other.example" }, platform::secret_text{ "other" }).status == platform::secret_status::ok));

	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "no_token"));
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(Git_NotARepository_VcsUnavailable_AndNoProject)
{
	not_a_repository = true;
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "not_a_repository"));
	BOOST_TEST(requests.empty());
	not_a_repository = false;

	branch_remote.clear();
	remote_urls.clear();
	const json::value no_remote = run("forge.pr_status", R"({"number":1})");
	BOOST_TEST(no_remote.as_object().contains("error"));
	BOOST_TEST(!is_error(no_remote, "no_token"));

	make_services(false);
	const cmd::result without_project = invoke("forge.pr_status", R"({"number":1})", cmd::origin::agent("0123abcd"));
	BOOST_TEST(!without_project.ok);
	BOOST_TEST(without_project.code == "no_project");
	BOOST_TEST(requests.empty());
}

BOOST_AUTO_TEST_CASE(PrStatus_ReadsOnePullRequest)
{
	answer(200, pr_created);

	const json::value result = run("forge.pr_status", R"({"number":42})");

	BOOST_TEST(result.as_object().at("number").as_int64() == 42);
	BOOST_TEST(text_of(result.as_object().at("state")) == "open");
	BOOST_REQUIRE(requests.size() == 1u);
	BOOST_TEST(requests[0].method == "GET");
	BOOST_TEST(requests[0].url == "https://api.github.com/repos/octo/engine/pulls/42");
	BOOST_TEST(tokens_seen[0] == canary);

	answer(404, R"({"message":"Not Found"})");
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":4242})"), "not_found"));
}

BOOST_AUTO_TEST_CASE(ListPrs_ReturnsTheFilteredList)
{
	answer(200, "[" + pr_created + "," + pr_created + "]");

	const json::value result = run("forge.list_prs", R"({"state":"closed","head":"agent/forge","base":"main","limit":5})");

	BOOST_REQUIRE(result.as_object().at("prs").is_array());
	BOOST_TEST(result.as_object().at("prs").as_array().size() == 2u);
	BOOST_TEST(result.as_object().at("prs").as_array()[0].as_object().at("number").as_int64() == 42);
	BOOST_REQUIRE(requests.size() == 1u);
	const std::string& url = requests[0].url;
	BOOST_TEST(url.find("state=closed") != std::string::npos);
	BOOST_TEST(url.find("per_page=5") != std::string::npos);
	BOOST_TEST(url.find("head=octo%3Aagent%2Fforge") != std::string::npos);
	BOOST_TEST(url.find("base=main") != std::string::npos);

	answer(200, "[]");
	const json::value empty = run("forge.list_prs", "{}");
	BOOST_TEST(empty.as_object().at("prs").as_array().empty());
	BOOST_TEST(requests.back().url.find("state=open") != std::string::npos);
	BOOST_TEST(requests.back().url.find("per_page=30") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ForgeErrors_AreMappedToTheirCodes)
{
	answer(401, R"({"message":"Bad credentials"})");
	BOOST_TEST(is_error(run("forge.create_pr", create_args()), "unauthorized"));
	answer(403, R"({"message":"Forbidden"})");
	BOOST_TEST(is_error(run("forge.create_pr", create_args()), "forbidden"));
	answer(422, R"({"message":"Validation Failed","errors":[{"message":"A pull request already exists"}]})");
	BOOST_TEST(is_error(run("forge.create_pr", create_args()), "validation"));
	answer(200, "not json");
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "invalid_response"));

	next_error = "could not connect";
	BOOST_TEST(is_error(run("forge.pr_status", R"({"number":1})"), "network"));
}

BOOST_AUTO_TEST_CASE(TheToken_IsInNoResultErrorLogOrRequestExceptAsTheBearer)
{
	std::vector<std::string> logged;
	const engine::log_sink_handle sink = engine::add_log_sink([&logged](std::string_view, engine::log_level, std::string_view message) {
		logged.emplace_back(message);
	});

	std::vector<json::value> results;
	results.push_back(run("forge.create_pr", create_args()));
	results.push_back(run("forge.pr_status", R"({"number":42})"));
	answer(200, "[" + pr_created + "]");
	results.push_back(run("forge.list_prs", "{}"));
	// The server echoes the credential back in its error text, the way a misbehaving proxy might.
	answer(401, R"({"message":"Bad credentials: )" + canary + R"("})");
	results.push_back(run("forge.create_pr", create_args()));
	next_error = "could not connect, sent Authorization: Bearer " + canary;
	results.push_back(run("forge.pr_status", R"({"number":1})"));
	next_error.clear();
	answer(200, R"({"number": 1, "title": ")" + canary + R"(", "state": "open", "draft": true, "html_url": "https://github.com/octo/engine/pull/1",
		"head": { "ref": "agent/forge" }, "base": { "ref": "main" }})");
	results.push_back(run("forge.pr_status", R"({"number":1})"));
	remote_urls = { { "origin", "https://evil.example/" + canary + "/engine.git" } };
	results.push_back(run("forge.pr_status", R"({"number":1})"));

	for (const json::value& result : results) BOOST_TEST(!has_canary(json::serialize(result)));
	for (const std::string& line : logged) BOOST_TEST(!has_canary(line));
	BOOST_REQUIRE(!requests.empty());
	for (std::size_t i = 0; i < requests.size(); ++i) {
		const edt::forge::http_request& request = requests[i];
		BOOST_TEST(tokens_seen[i] == canary);
		BOOST_TEST(!has_canary(request.method));
		BOOST_TEST(!has_canary(request.url));
		BOOST_TEST(!has_canary(request.body));
		for (const auto& [name, value] : request.headers) {
			BOOST_TEST(!has_canary(name));
			BOOST_TEST(!has_canary(value));
		}
	}
	for (const std::vector<std::string>& args : git_calls) {
		for (const std::string& arg : args) BOOST_TEST(!has_canary(arg));
	}
}

BOOST_AUTO_TEST_CASE(Cancel_StopsTheRequest_AndTheJobEndsCancelled)
{
	std::optional<edt::job_service::job_id> job;
	bool saw_stop = false;
	on_http = [this, &job, &saw_stop](std::stop_token stop) {
		if (!job) return;
		BOOST_TEST(jobs->cancel(*job));
		saw_stop = stop.stop_requested();
	};
	const cmd::result started = invoke("forge.create_pr", create_args(), cmd::origin::agent("0123abcd"));
	BOOST_REQUIRE(started.ok);
	job = static_cast<edt::job_service::job_id>(started.value.as_object().at("job").as_int64());

	run_to_end();

	BOOST_TEST(saw_stop);
	const auto info = jobs->info(*job);
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST((info->status == edt::job_status::cancelled));
}

BOOST_AUTO_TEST_CASE(Metadata_CategoryRiskAndTheDefaultModeAreAsk)
{
	const edt::policy_layer none{};
	for (const char* id : { "forge.create_pr", "forge.pr_status", "forge.list_prs" }) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* desc = commands.find(id);
			BOOST_REQUIRE(desc != nullptr);
			BOOST_TEST((desc->category == cmd::agent_category::vcs_forge));
			BOOST_TEST(desc->starts_job);
			BOOST_TEST(desc->expose_as_tool);
			BOOST_TEST(((edt::effective_mode(*desc, none, none).mode) == cmd::policy_mode::ask));
			BOOST_TEST(((edt::default_mode(*desc)) == cmd::policy_mode::ask));
		}
	}
	const cmd::command_desc* create = commands.find("forge.create_pr");
	BOOST_TEST(create->mutates);
	BOOST_TEST((create->risk == cmd::danger::confirm));
	BOOST_TEST(!commands.find("forge.pr_status")->mutates);
	BOOST_TEST(!commands.find("forge.list_prs")->mutates);

	// A category entry loosens the two reads; the command that confirms stays at ask until its own entry says otherwise.
	edt::policy_layer allowing;
	allowing.categories[cmd::agent_category::vcs_forge] = cmd::policy_mode::allow;
	BOOST_TEST(((edt::effective_mode(*commands.find("forge.pr_status"), allowing, none).mode) == cmd::policy_mode::allow));
	BOOST_TEST(((edt::effective_mode(*commands.find("forge.list_prs"), allowing, none).mode) == cmd::policy_mode::allow));
	BOOST_TEST(((edt::effective_mode(*create, allowing, none).mode) == cmd::policy_mode::ask));
	allowing.commands["forge.create_pr"] = cmd::policy_mode::allow;
	BOOST_TEST(((edt::effective_mode(*create, allowing, none).mode) == cmd::policy_mode::allow));
	edt::policy_layer denying;
	denying.categories[cmd::agent_category::vcs_forge] = cmd::policy_mode::deny;
	BOOST_TEST(((edt::effective_mode(*commands.find("forge.pr_status"), denying, none).mode) == cmd::policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(TheTokenHasNoCommand_AndTheAgentSeesExactlyThreeForgeCommands)
{
	std::vector<std::string> visible;
	for (const cmd::command_desc* desc : commands.list()) {
		const std::string& id = desc->id;
		BOOST_TEST(id.find("token") == std::string::npos, id);
		BOOST_TEST(id.find("secret") == std::string::npos, id);
		BOOST_TEST(id.find("credential") == std::string::npos, id);
		const bool agent_visible = desc->expose_as_tool && desc->category != cmd::agent_category::unset
			&& desc->category != cmd::agent_category::not_for_agents;
		if (agent_visible && id.starts_with("forge.")) visible.push_back(id);
		// Not even a description or a schema names a token as an argument.
		if (id.starts_with("forge.")) BOOST_TEST(json::serialize(desc->args_schema).find("token") == std::string::npos, id);
	}
	std::sort(visible.begin(), visible.end());
	const std::vector<std::string> expected{ "forge.create_pr", "forge.list_prs", "forge.pr_status" };
	BOOST_TEST((visible == expected));
	BOOST_TEST(commands.list().size() == 3u);
}

BOOST_AUTO_TEST_SUITE_END()
