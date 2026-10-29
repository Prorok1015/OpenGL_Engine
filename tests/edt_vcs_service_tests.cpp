#include <boost/test/unit_test.hpp>
#include "edt_vcs_service.h"
#include "edt_path_text.h"
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_service_commands.h>
#include <snk/platform/plt_process.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
extern char** environ;
#endif

// vcs_service (#454) on a manual task_system: a real repository made with the git on PATH (CI has it), and a fake
// runner for the error paths. The folders are outside the source tree and git does not look above them
// (GIT_CEILING_DIRECTORIES), so "not a repository" is not answered by the engine's own checkout.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace std::string_literals;

// ü: MSVC reads the source in the local code page, so non-ASCII text is spelled in escapes.
const std::string u_umlaut = "\xC3\xBC";

fs::path path_of(std::string_view utf8)
{
	return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string text(const json::value& value)
{
	return std::string(value.as_string());
}

std::vector<std::string> parent_environment()
{
	std::vector<std::string> out;
#ifdef _WIN32
	wchar_t* block = GetEnvironmentStringsW();
	if (block == nullptr) return out;
	for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wstring_view{ entry }.size() + 1) {
		out.push_back(edt::utf8_of(fs::path{ std::wstring_view{ entry } }));
	}
	FreeEnvironmentStringsW(block);
#else
	for (char** entry = environ; *entry != nullptr; ++entry) out.emplace_back(*entry);
#endif
	return out;
}

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

struct vcs_fixture
{
	vcs_fixture()
	{
		ds::task_system_config config;
		config.mode = ds::policy::manual;
		tasks = std::make_unique<ds::task_system>(config);
		jobs = std::make_unique<edt::job_service>(*tasks, &events);
		static int counter = 0;
		base = fs::temp_directory_path() / std::format("snk_edt_vcs_tests_{}_{}",
			std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
		fs::create_directories(base);
		environment = parent_environment();
		edt::set_variable(environment, "GIT_CEILING_DIRECTORIES", edt::utf8_of(base));
	}

	~vcs_fixture()
	{
		registrations.clear();
		service.reset();
		jobs->cancel_all_and_wait();
		std::error_code ec;
		fs::remove_all(base, ec);
	}

	void run_to_end()
	{
		while (tasks->run_one(ds::lane::io)) {}
		tasks->drain_main();
	}

	// The result of a job that must end `done`.
	json::value finish(edt::job_service::job_id id)
	{
		run_to_end();
		const auto info = jobs->info(id);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE_MESSAGE(info->status == edt::job_status::done,
			std::format("job ended {}: {}", edt::job_status_name(info->status), info->error));
		BOOST_REQUIRE(info->result.has_value());
		return *info->result;
	}

	json::value finish(const edt::vcs_service::started& started)
	{
		BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), started.code + ": " + started.error);
		return finish(started.job);
	}

	// The real git, found the way the service finds it.
	fs::path real_git()
	{
		const auto found = edt::find_on_path(environment, edt::k_native_env == edt::env_flavor::windows ? "git.exe" : "git");
		BOOST_REQUIRE_MESSAGE(found.has_value(), "these tests need git on PATH");
		return *found;
	}

	void git(const fs::path& dir, const std::vector<std::string>& args)
	{
		std::string output;
		platform::process_options options;
		options.working_dir = dir;
		options.environment = environment;
		const auto result = platform::run_process(real_git(), args, options,
			[&output](std::string_view line) { output.append(line).push_back('\n'); }, {});
		BOOST_REQUIRE_MESSAGE(result.error.empty() && result.exit_code == 0,
			std::format("git {}: {} (exit {}) {}", args.empty() ? std::string{} : args.front(), result.error, result.exit_code, output));
	}

	void write(const fs::path& file, std::string_view content)
	{
		fs::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
	}

	// <base>/repo: two commits on main, a branch `feature`, a.txt modified, a staged rename, an untracked UTF-8 file.
	fs::path make_repository()
	{
		const fs::path repo = base / "repo";
		fs::create_directories(repo);
		git(repo, { "init", "-q" });
		git(repo, { "symbolic-ref", "HEAD", "refs/heads/main" });
		git(repo, { "config", "user.name", "Test User" });
		git(repo, { "config", "user.email", "test@example.com" });
		git(repo, { "config", "commit.gpgsign", "false" });
		git(repo, { "config", "core.autocrlf", "false" });
		write(repo / "a.txt", "one\n");
		git(repo, { "add", "a.txt" });
		git(repo, { "commit", "-q", "-m", "first commit" });
		write(repo / "dir with space" / "b.txt", "bee\n");
		git(repo, { "add", "--", "dir with space/b.txt" });
		git(repo, { "commit", "-q", "-m", "second: b" });
		git(repo, { "branch", "feature" });
		write(repo / "a.txt", "one\ntwo\n");
		git(repo, { "mv", "dir with space/b.txt", "dir with space/c.txt" });
		write(repo / path_of("new " + u_umlaut + ".txt"), "new\n");
		return repo;
	}

	void make_service(edt::vcs_options options)
	{
		service = std::make_unique<edt::vcs_service>(*jobs, std::move(options));
	}

	edt::vcs_options real_options(std::optional<fs::path> project)
	{
		edt::vcs_options options;
		options.project_root = [project] { return project; };
		options.environment = environment;
		return options;
	}

	edt::editor_events events;
	std::unique_ptr<ds::task_system> tasks;
	std::unique_ptr<edt::job_service> jobs;
	fs::path base;
	std::vector<std::string> environment;
	std::unique_ptr<edt::vcs_service> service;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
};

// A fake git: --version and rev-parse answer as git would, every other call goes to `command`.
struct fake_git_fixture : vcs_fixture
{
	edt::vcs_options fake_options()
	{
		edt::vcs_options options;
		options.git = "fake-git";
		options.project_root = [this] { return std::optional<fs::path>{ base }; };
		options.environment = std::vector<std::string>{ "PATH=/nowhere" };
		options.run = [this](const fs::path&, const std::vector<std::string>& args, const platform::process_options& opts,
			const std::function<void(std::string_view)>& on_line, std::stop_token stop) -> platform::process_result {
			calls.push_back(args);
			last_options = opts;
			if (has_arg(args, "--version")) {
				on_line("git version 2.45.1\n");
				return exited(0);
			}
			if (has_arg(args, "rev-parse")) {
				on_line(edt::utf8_of(base) + "\n");
				return exited(0);
			}
			if (has_arg(args, "--get-regexp")) {
				if (filter_config.empty()) return exited(1);
				on_line(filter_config);
				return exited(0);
			}
			return command(args, opts, on_line, stop);
		};
		return options;
	}

	std::vector<std::vector<std::string>> calls;
	platform::process_options last_options;
	// What the listing of the filter drivers prints; empty: none (exit 1).
	std::string filter_config;
	std::function<platform::process_result(const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token)> command;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtVcsServiceTests, vcs_fixture)

BOOST_AUTO_TEST_CASE(RealRepository_InfoStatusLogBranchesAndDiff)
{
	const fs::path repo = make_repository();
	make_service(real_options(repo));

	const auto& state = service->info();
	BOOST_REQUIRE_MESSAGE(state.available, state.reason);
	BOOST_REQUIRE(state.root.has_value());
	std::error_code ec;
	BOOST_TEST(fs::equivalent(*state.root, repo, ec));
	BOOST_TEST(!state.git_version.empty());

	const json::value status = finish(service->start_status());
	BOOST_TEST_INFO("status: " << json::serialize(status));
	const json::object& branch = status.as_object().at("branch").as_object();
	BOOST_TEST(text(branch.at("head")) == "main");
	BOOST_TEST(!status.as_object().at("clean").as_bool());
	const json::array& entries = status.as_object().at("entries").as_array();
	const auto entry = [&entries](std::string_view path) -> const json::object* {
		for (const auto& item : entries) {
			if (item.as_object().at("path").as_string() == path) return &item.as_object();
		}
		return nullptr;
	};
	const json::object* modified = entry("a.txt");
	BOOST_REQUIRE(modified != nullptr);
	BOOST_TEST(text(modified->at("worktree")) == "M");
	const json::object* renamed = entry("dir with space/c.txt");
	BOOST_REQUIRE(renamed != nullptr);
	BOOST_TEST(text(renamed->at("kind")) == "renamed");
	BOOST_TEST(text(renamed->at("orig_path")) == "dir with space/b.txt");
	const json::object* untracked = entry("new " + u_umlaut + ".txt");
	BOOST_REQUIRE(untracked != nullptr);
	BOOST_TEST(text(untracked->at("kind")) == "untracked");

	const json::value page = finish(service->start_log({}, {}, 1, {}));
	const json::array& first = page.as_object().at("commits").as_array();
	BOOST_REQUIRE_EQUAL(first.size(), 1u);
	BOOST_TEST(text(first[0].as_object().at("subject")) == "second: b");
	BOOST_TEST(text(first[0].as_object().at("author")) == "Test User");
	BOOST_TEST(text(first[0].as_object().at("email")) == "test@example.com");
	BOOST_TEST(first[0].as_object().at("time").as_int64() > 0);
	BOOST_REQUIRE(page.as_object().contains("next_cursor"));
	const json::value next = finish(service->start_log({}, {}, 1, text(page.as_object().at("next_cursor"))));
	const json::array& second = next.as_object().at("commits").as_array();
	BOOST_REQUIRE_EQUAL(second.size(), 1u);
	BOOST_TEST(text(second[0].as_object().at("subject")) == "first commit");
	BOOST_TEST(!next.as_object().contains("next_cursor"));
	const auto parents = first[0].as_object().at("parents").as_array();
	BOOST_REQUIRE_EQUAL(parents.size(), 1u);
	BOOST_TEST(text(parents[0]) == text(second[0].as_object().at("hash")));

	const json::value of_path = finish(service->start_log("main", "a.txt", 50, {}));
	BOOST_TEST(of_path.as_object().at("commits").as_array().size() == 1u);

	const json::value branches = finish(service->start_branches());
	BOOST_TEST(text(branches.as_object().at("current")) == "main");
	const json::array& listed = branches.as_object().at("branches").as_array();
	BOOST_TEST(listed.size() == 2u);
	BOOST_TEST(std::any_of(listed.begin(), listed.end(), [](const json::value& b) {
		return b.as_object().at("name").as_string() == "feature" && !b.as_object().at("head").as_bool();
	}));

	const json::value worktree = finish(service->start_diff("a.txt", false));
	BOOST_TEST(!worktree.as_object().at("truncated").as_bool());
	BOOST_TEST(text(worktree.as_object().at("text")).find("+two") != std::string::npos);
	const json::value staged = finish(service->start_diff({}, true));
	BOOST_TEST(text(staged.as_object().at("text")).find("c.txt") != std::string::npos);
	BOOST_TEST(text(staged.as_object().at("text")).find("+two") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(RealRepository_ThroughTheCommands)
{
	const fs::path repo = make_repository();
	make_service(real_options(repo));
	registrations = edt::register_vcs_commands(commands, *service);
	BOOST_TEST(registrations.size() == 5u);

	const cmd::result info = commands.invoke("vcs.info", json::object{}, cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(info.ok, info.error);
	BOOST_TEST(info.value.as_object().at("available").as_bool());
	BOOST_TEST(info.value.as_object().contains("root"));

	const cmd::result started = commands.invoke("vcs.status", json::object{}, cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto job = started.value.as_object().at("job").as_int64();
	const json::value status = finish(static_cast<edt::job_service::job_id>(job));
	BOOST_TEST(status.as_object().contains("branch"));

	BOOST_TEST(commands.invoke("vcs.log", json::parse(R"({"limit":500})"), cmd::origin::test()).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("vcs.log", json::parse(R"({"ref":"--output=x"})"), cmd::origin::test()).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("vcs.log", json::parse(R"({"cursor":"abc"})"), cmd::origin::test()).code == cmd::codes::invalid_args);
	const cmd::result diff = commands.invoke("vcs.diff", json::parse(R"({"path":"a.txt","staged":false})"), cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(diff.ok, diff.error);
	const json::value diff_result = finish(static_cast<edt::job_service::job_id>(diff.value.as_object().at("job").as_int64()));
	BOOST_TEST(text(diff_result.as_object().at("text")).find("+two") != std::string::npos);

	for (const char* id : { "vcs.info", "vcs.status", "vcs.diff", "vcs.log", "vcs.branches" }) {
		const cmd::command_desc* desc = commands.find(id);
		BOOST_REQUIRE(desc != nullptr);
		BOOST_TEST(!desc->mutates, id);
		BOOST_TEST(!desc->long_running, id);
		BOOST_TEST((desc->risk == cmd::danger::none), id);
	}
}

BOOST_AUTO_TEST_CASE(RealRepository_WithoutCommits_HasAnEmptyLog)
{
	const fs::path repo = base / "empty";
	fs::create_directories(repo);
	git(repo, { "init", "-q" });
	make_service(real_options(repo));

	const json::value log = finish(service->start_log({}, {}, 10, {}));
	BOOST_TEST(log.as_object().at("commits").as_array().empty());
	const json::value status = finish(service->start_status());
	BOOST_TEST(text(status.as_object().at("branch").as_object().at("oid")).empty());
}

BOOST_AUTO_TEST_CASE(RealRepository_CancelledJob_EndsCancelledWithoutAResult)
{
	const fs::path repo = make_repository();
	make_service(real_options(repo));

	const auto started = service->start_status();
	BOOST_REQUIRE(static_cast<bool>(started));
	BOOST_TEST(jobs->cancel(started.job));
	run_to_end();

	const auto info = jobs->info(started.job);
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST((info->status == edt::job_status::cancelled));
	BOOST_TEST(!info->result.has_value());
}

// vcs.info says so at once; the other commands still answer {job}, and git's own error is the job's result, seen
// through job.get and job_finished.
BOOST_AUTO_TEST_CASE(NotARepository_IsReportedAndTheJobResultCarriesTheError)
{
	const fs::path plain = base / "plain";
	fs::create_directories(plain);
	make_service(real_options(plain));

	const auto& state = service->info();
	BOOST_TEST(!state.available);
	BOOST_TEST(state.code == "not_a_repository");
	BOOST_TEST(state.reason == "not a repository");
	BOOST_TEST(!state.git_version.empty());

	registrations = edt::register_vcs_commands(commands, *service);
	for (auto& registration : edt::register_job_commands(commands, *jobs)) registrations.push_back(std::move(registration));
	auto finished = std::make_shared<std::vector<std::optional<json::value>>>();
	const auto connection = events.job_finished.subscribe([finished](std::uint64_t, edt::job_status, const std::string&,
		const std::optional<json::value>& result) { finished->push_back(result); });

	const cmd::result started = commands.invoke("vcs.log", json::object{}, cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const std::int64_t job = started.value.as_object().at("job").as_int64();
	run_to_end();

	const cmd::result got = commands.invoke("job.get", json::object{ { "id", job } }, cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(got.ok, got.code + ": " + got.error);
	BOOST_TEST(text(got.value.as_object().at("status")) == "done");
	const json::object& error = got.value.as_object().at("result").as_object().at("error").as_object();
	BOOST_TEST(text(error.at("code")) == "not_a_repository");
	BOOST_TEST(text(error.at("message")).find("not a git repository") != std::string::npos);
	BOOST_TEST(text(error.at("stderr_tail")).find("not a git repository") != std::string::npos);

	BOOST_REQUIRE_EQUAL(finished->size(), 1u);
	BOOST_REQUIRE(finished->front().has_value());
	BOOST_TEST(text(finished->front()->as_object().at("error").as_object().at("code")) == "not_a_repository");
}

// A project unzipped with a hostile .git/config: an fsmonitor hook, a diff textconv and command driver and a clean
// filter, all a script that leaves a marker. Plain git runs them; the reads of the service run none.
BOOST_AUTO_TEST_CASE(AHostileRepositoryConfig_RunsNothingOnReads)
{
	const fs::path repo = make_repository();
	const fs::path marker = base / "ran.txt";
	const fs::path script = base / "evil.sh";
	const auto generic = [](const fs::path& path) {
		const std::u8string utf8 = path.generic_u8string();
		return std::string(utf8.begin(), utf8.end());
	};
	const std::string script_text = generic(script);
	// Git (for Windows too) runs these through its sh; forward slashes keep the paths intact there.
	write(script, "#!/bin/sh\necho ran >> '" + generic(marker) + "'\ncat\n");
#ifndef _WIN32
	fs::permissions(script, fs::perms::owner_all, fs::perm_options::add);
#endif
	write(repo / ".gitattributes", "*.txt diff=evil filter=evil\n");
	git(repo, { "config", "core.fsmonitor", script_text });
	git(repo, { "config", "diff.evil.textconv", script_text });
	git(repo, { "config", "diff.evil.command", script_text });
	git(repo, { "config", "filter.evil.clean", script_text });
	git(repo, { "config", "filter.evil.smudge", script_text });

	git(repo, { "diff" });
	git(repo, { "status" });
	std::error_code ec;
#ifdef _WIN32
	BOOST_WARN_MESSAGE(fs::exists(marker, ec), "plain git ran none of the scripts here: the check below proves less");
#else
	BOOST_REQUIRE_MESSAGE(fs::exists(marker, ec), "plain git must run the hostile config, or this test proves nothing");
#endif
	fs::remove(marker, ec);

	make_service(real_options(repo));
	BOOST_REQUIRE_MESSAGE(service->info().available, service->info().reason);
	const json::value status = finish(service->start_status());
	BOOST_TEST(!status.as_object().contains("error"), json::serialize(status));
	const json::value unstaged = finish(service->start_diff({}, false));
	BOOST_TEST(text(unstaged.as_object().at("text")).find("+two") != std::string::npos, json::serialize(unstaged));
	const json::value staged = finish(service->start_diff({}, true));
	BOOST_TEST(!staged.as_object().contains("error"), json::serialize(staged));
	finish(service->start_diff("a.txt", false));
	finish(service->start_log({}, {}, 10, {}));
	finish(service->start_log({}, "a.txt", 10, {}));
	finish(service->start_branches());

	BOOST_TEST(!fs::exists(marker, ec), "a read ran a program of the repository's config");
}

// Linux and macOS only: Windows names hold no newline, and are UTF-16, so no byte can be invalid UTF-8.
BOOST_AUTO_TEST_CASE(RealRepository_ANewlineAndANonUtf8Name_KeepTheOtherEntries)
{
#ifdef _WIN32
	BOOST_TEST_MESSAGE("skipped on Windows: a file name cannot hold a newline or a byte that is not UTF-8");
#else
	const fs::path repo = base / "names";
	fs::create_directories(repo);
	git(repo, { "init", "-q" });
	write(repo / "line\nbreak.txt", "1\n");
	write(repo / fs::path(std::string("bad\xFF") + "name.txt"), "2\n");
	write(repo / "zz after.txt", "3\n");
	make_service(real_options(repo));

	const json::value status = finish(service->start_status());
	BOOST_TEST_INFO("status: " << json::serialize(status));
	const json::array& entries = status.as_object().at("entries").as_array();
	BOOST_TEST(entries.size() == 3u);
	const auto has_path = [&entries](std::string_view path) {
		return std::any_of(entries.begin(), entries.end(), [path](const json::value& e) {
			return e.as_object().at("path").as_string() == path && e.as_object().at("kind").as_string() == "untracked";
		});
	};
	BOOST_TEST(has_path("line\nbreak.txt"));
	BOOST_TEST(has_path("bad\xEF\xBF\xBD" "name.txt"));
	BOOST_TEST(has_path("zz after.txt"));
#endif
}

BOOST_AUTO_TEST_CASE(NoProject_IsReported)
{
	make_service(real_options(std::nullopt));
	BOOST_TEST(!service->info().available);
	BOOST_TEST(service->info().code == "no_project");
	BOOST_TEST(service->start_branches().code == "no_project");
}

BOOST_AUTO_TEST_CASE(GitNotFound_ClearErrorAndNoJob)
{
	edt::vcs_options options = real_options(base);
	options.git = base / "no_such_dir" / "git";
	make_service(std::move(options));

	const auto& state = service->info();
	BOOST_TEST(!state.available);
	BOOST_TEST(state.code == "vcs_unavailable");
	BOOST_TEST(state.reason.starts_with("git not found"), state.reason);
	const auto started = service->start_status();
	BOOST_TEST(!started);
	BOOST_TEST(started.code == "vcs_unavailable");

	registrations = edt::register_vcs_commands(commands, *service);
	const cmd::result info = commands.invoke("vcs.info", json::object{}, cmd::origin::test());
	BOOST_REQUIRE(info.ok);
	BOOST_TEST(!info.value.as_object().at("available").as_bool());
	BOOST_TEST(text(info.value.as_object().at("reason")).starts_with("git not found"));
	BOOST_TEST(!info.value.as_object().contains("git_version"));
	const cmd::result status = commands.invoke("vcs.status", json::object{}, cmd::origin::test());
	BOOST_TEST(!status.ok);
	BOOST_TEST(status.code == "vcs_unavailable");
	BOOST_TEST(jobs->list().empty());
}

BOOST_AUTO_TEST_CASE(GitNotOnPath_IsGitNotFound)
{
	edt::vcs_options options = real_options(base);
	options.environment = std::vector<std::string>{ "PATH=" + edt::utf8_of(base / "empty") };
	make_service(std::move(options));
	BOOST_TEST(!service->info().available);
	BOOST_TEST(service->info().reason == "git not found");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtVcsFakeGitTests, fake_git_fixture)

BOOST_AUTO_TEST_CASE(EveryCall_GetsTheFixedOptionsAndEnvironment)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		on_line(std::string_view("# branch.head main\0", 19));
		return exited(0);
	};
	make_service(fake_options());
	BOOST_REQUIRE(service->info().available);
	BOOST_TEST(service->info().git_version == "2.45.1");

	finish(service->start_status());
	const auto& args = calls.back();
	BOOST_REQUIRE(args.size() >= 5u);
	BOOST_TEST(args[0] == "--no-pager");
	const auto subcommand = std::find(args.begin(), args.end(), "status");
	for (const char* config : { "core.quotepath=off", "core.fsmonitor=false", "diff.external=", "log.showSignature=false",
			"status.submoduleSummary=false", "diff.ignoreSubmodules=all", "filter.lfs.clean=", "filter.lfs.smudge=",
			"filter.lfs.process=", "filter.lfs.required=false" }) {
		BOOST_TEST_CONTEXT(config) {
			const auto found = std::find(args.begin(), args.end(), config);
			BOOST_REQUIRE(found != args.end());
			BOOST_TEST(*std::prev(found) == "-c");
			BOOST_TEST((found < subcommand));
		}
	}
	BOOST_TEST(std::any_of(args.begin(), args.end(), [](const std::string& a) { return a.starts_with("core.hooksPath="); }));
	BOOST_TEST(has_arg(args, "--ignore-submodules=all"));
	BOOST_TEST(has_arg(args, "--no-optional-locks"));
	BOOST_TEST(has_arg(args, "--literal-pathspecs"));
	BOOST_TEST(has_arg(args, "--porcelain=v2"));
	BOOST_TEST(has_arg(args, "-z"));
	BOOST_TEST(last_options.raw_output);
	BOOST_TEST(static_cast<bool>(last_options.on_stderr_line));
	std::error_code ec;
	BOOST_TEST(fs::equivalent(last_options.working_dir, base, ec));
	BOOST_REQUIRE(last_options.environment.has_value());
	const auto& env = *last_options.environment;
	BOOST_TEST(edt::variable_of(env, "LC_ALL").value_or("") == "C");
	BOOST_TEST(edt::variable_of(env, "GIT_TERMINAL_PROMPT").value_or("") == "0");
	BOOST_TEST(edt::variable_of(env, "GCM_INTERACTIVE").value_or("") == "never");
	BOOST_TEST(edt::variable_of(env, "GIT_SSH_COMMAND").value_or("") == "ssh -oBatchMode=yes");
	BOOST_TEST(edt::variable_of(env, "PATH").value_or("") == "/nowhere");
}

BOOST_AUTO_TEST_CASE(AUserGitSshCommand_IsKept)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	auto options = fake_options();
	options.environment->push_back("GIT_SSH_COMMAND=ssh -i key");
	make_service(std::move(options));
	finish(service->start_branches());
	BOOST_TEST(edt::variable_of(*last_options.environment, "GIT_SSH_COMMAND").value_or("") == "ssh -i key");
}

BOOST_AUTO_TEST_CASE(AFailure_IsAResultWithTheClassifiedError)
{
	command = [](const std::vector<std::string>&, const platform::process_options& opts,
		const std::function<void(std::string_view)>&, std::stop_token) {
		opts.on_stderr_line("fatal: could not read Username for 'https://example.com': terminal prompts disabled");
		return exited(128);
	};
	make_service(fake_options());

	const json::value result = finish(service->start_status());
	const json::object& error = result.as_object().at("error").as_object();
	BOOST_TEST(text(error.at("code")) == "auth_required");
	BOOST_TEST(text(error.at("message")).find("terminal prompts disabled") != std::string::npos);
	BOOST_TEST(text(error.at("stderr_tail")).find("terminal prompts disabled") != std::string::npos);
	BOOST_TEST(error.at("exit_code").as_int64() == 128);
}

BOOST_AUTO_TEST_CASE(AFailure_StderrTailKeepsTheLast20Lines_MessageTheFatalLine)
{
	command = [](const std::vector<std::string>&, const platform::process_options& opts,
		const std::function<void(std::string_view)>&, std::stop_token) {
		for (int i = 0; i < 30; ++i) opts.on_stderr_line(std::format("hint: line {}", i));
		opts.on_stderr_line("fatal: bad revision 'nope'");
		opts.on_stderr_line(std::string(5000, 'x'));
		return exited(128);
	};
	make_service(fake_options());

	const json::value result = finish(service->start_log("nope", {}, 10, {}));
	const json::object& error = result.as_object().at("error").as_object();
	BOOST_TEST(text(error.at("code")) == "bad_revision");
	BOOST_TEST(text(error.at("message")) == "fatal: bad revision 'nope'");
	const std::string tail = text(error.at("stderr_tail"));
	BOOST_TEST(tail.size() <= 4096u);
	BOOST_TEST(tail.ends_with("xxx"));

	command = [](const std::vector<std::string>&, const platform::process_options& opts,
		const std::function<void(std::string_view)>&, std::stop_token) {
		for (int i = 0; i < 30; ++i) opts.on_stderr_line(std::format("warning: line {}", i));
		return exited(1);
	};
	const json::value short_lines = finish(service->start_status());
	const std::string short_tail = text(short_lines.as_object().at("error").as_object().at("stderr_tail"));
	BOOST_TEST(short_tail.starts_with("warning: line 10"));
	BOOST_TEST(short_tail.ends_with("warning: line 29"));
	BOOST_TEST(std::count(short_tail.begin(), short_tail.end(), '\n') == 19);
}

namespace {
	// A runner call that hangs until it is stopped, as a git waiting on a network drive would.
	platform::process_result hang_until_stopped(std::stop_token stop)
	{
		const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < give_up) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		platform::process_result result = exited(-1);
		result.cancelled = stop.stop_requested();
		return result;
	}
}

BOOST_AUTO_TEST_CASE(AVersionProbeThatHangs_TimesOutAndTurnsVersionControlOff)
{
	auto options = fake_options();
	options.probe_timeout = std::chrono::milliseconds(100);
	options.run = [](const fs::path&, const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token stop) { return hang_until_stopped(stop); };

	const auto before = std::chrono::steady_clock::now();
	make_service(std::move(options));
	BOOST_TEST((std::chrono::steady_clock::now() - before < std::chrono::seconds(30)));

	BOOST_TEST(!service->info().available);
	BOOST_TEST(service->info().code == "vcs_unavailable");
	BOOST_TEST(service->info().reason == "git did not answer within 100 ms");
	BOOST_TEST(service->start_status().code == "vcs_unavailable");
	BOOST_TEST(jobs->list().empty());
}

BOOST_AUTO_TEST_CASE(ARevParseThatHangs_TimesOutAndTurnsVersionControlOff)
{
	auto options = fake_options();
	options.probe_timeout = std::chrono::milliseconds(100);
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	options.run = [](const fs::path&, const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		if (has_arg(args, "--version")) {
			on_line("git version 2.45.1\n");
			return exited(0);
		}
		return hang_until_stopped(stop);
	};
	make_service(std::move(options));

	BOOST_TEST(!service->info().available);
	BOOST_TEST(service->info().git_version == "2.45.1");
	BOOST_TEST(service->info().code == "vcs_unavailable");
	BOOST_TEST(service->info().reason.starts_with("git did not answer within"));
	BOOST_TEST(service->start_branches().code == "vcs_unavailable");
}

BOOST_AUTO_TEST_CASE(AGitThatCannotStart_IsVcsUnavailable)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) {
		platform::process_result result;
		result.error = "cannot start 'fake-git': No such file or directory";
		return result;
	};
	make_service(fake_options());
	const json::value result = finish(service->start_diff({}, false));
	BOOST_TEST(text(result.as_object().at("error").as_object().at("code")) == "vcs_unavailable");
}

BOOST_AUTO_TEST_CASE(Diff_IsCutAtTheCapAndGitIsStopped)
{
	bool stopped = false;
	command = [&stopped](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		BOOST_TEST(has_arg(args, "--no-ext-diff"));
		BOOST_TEST(has_arg(args, "--no-textconv"));
		BOOST_TEST(has_arg(args, "--cached"));
		BOOST_TEST(args.back() == "some file.txt");
		const std::string chunk(64 * 1024, 'x');
		for (int i = 0; i < 8 && !stop.stop_requested(); ++i) on_line(chunk);
		stopped = stop.stop_requested();
		platform::process_result result = exited(143);
		result.cancelled = stopped;
		return result;
	};
	make_service(fake_options());

	const json::value result = finish(service->start_diff("some file.txt", true));
	BOOST_TEST(stopped);
	BOOST_TEST(result.as_object().at("truncated").as_bool());
	BOOST_TEST(result.as_object().at("text").as_string().size() == edt::vcs_service::k_diff_cap);
}

BOOST_AUTO_TEST_CASE(Log_PassesThePageAndClampsTheLimit)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	make_service(fake_options());

	finish(service->start_log("main", "src/a b.cpp", 500, "40"));
	const auto& args = calls.back();
	BOOST_TEST(has_arg(args, "--skip=40"));
	BOOST_TEST(has_arg(args, "--max-count=201"));
	BOOST_TEST(has_arg(args, "main"));
	BOOST_REQUIRE(args.size() >= 2u);
	BOOST_TEST(args[args.size() - 2] == "--");
	BOOST_TEST(args.back() == "src/a b.cpp");
	BOOST_TEST(args[args.size() - 4] == "--end-of-options");
	BOOST_TEST(args[args.size() - 3] == "main");

	finish(service->start_log({}, {}, 10, "99999999999999999999999"));
	const auto& plain = calls.back();
	BOOST_TEST(has_arg(plain, "--skip=1000000000"));
	BOOST_TEST(plain.back() == "--");
	BOOST_TEST(plain[plain.size() - 2] == "--end-of-options");

	// A ref that looks like an option never reaches git.
	const std::size_t before = calls.size();
	BOOST_TEST(service->start_log("-p", {}, 10, {}).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_log("--output=/tmp/x", {}, 10, {}).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_log({}, {}, 10, "-1").code == cmd::codes::invalid_args);
	run_to_end();
	BOOST_TEST(calls.size() == before);
}

BOOST_AUTO_TEST_CASE(Status_IsCappedAtFourMiB_AndKeepsOnlyWholeRecords)
{
	bool stopped = false;
	command = [&stopped](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		on_line(std::string_view("# branch.head main\0", 19));
		std::string record = "? " + std::string(1000, 'f');
		record.push_back('\0');
		for (int i = 0; i < 6000 && !stop.stop_requested(); ++i) on_line(record);
		stopped = stop.stop_requested();
		platform::process_result result = exited(143);
		result.cancelled = stopped;
		return result;
	};
	make_service(fake_options());

	const json::value result = finish(service->start_status());
	BOOST_TEST(stopped);
	BOOST_TEST(result.as_object().at("truncated").as_bool());
	const json::array& entries = result.as_object().at("entries").as_array();
	BOOST_TEST(!entries.empty());
	BOOST_TEST(entries.size() < 6000u);
	for (const auto& entry : entries) {
		BOOST_REQUIRE_EQUAL(entry.as_object().at("path").as_string().size(), 1000u);
	}
}

BOOST_AUTO_TEST_CASE(Branches_AreCappedAtFourMiB)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		const std::string line = "refs/heads/" + std::string(1000, 'b') + std::string("\0aaaa\0\0 \n", 9);
		for (int i = 0; i < 6000 && !stop.stop_requested(); ++i) on_line(line);
		platform::process_result result = exited(143);
		result.cancelled = stop.stop_requested();
		return result;
	};
	make_service(fake_options());
	const json::value result = finish(service->start_branches());
	BOOST_TEST(result.as_object().at("truncated").as_bool());
	const json::array& branches = result.as_object().at("branches").as_array();
	BOOST_TEST(!branches.empty());
	for (const auto& branch : branches) {
		BOOST_REQUIRE_EQUAL(branch.as_object().at("name").as_string().size(), 1000u);
	}
}

BOOST_AUTO_TEST_CASE(FilterDrivers_OfTheConfig_AreSwitchedOffOnEveryCall)
{
	filter_config = "filter.evil.clean\n./run-me\0filter.evil.smudge\nx\0filter.with.dot.process\ny\0"s;
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	make_service(fake_options());

	finish(service->start_diff({}, false));
	const auto& args = calls.back();
	for (const char* config : { "filter.evil.clean=", "filter.evil.smudge=", "filter.evil.process=",
			"filter.evil.required=false", "filter.with.dot.clean=", "filter.with.dot.process=", "filter.lfs.clean=" }) {
		BOOST_TEST_CONTEXT(config) {
			BOOST_TEST(has_arg(args, config));
		}
	}
	BOOST_TEST(std::count(args.begin(), args.end(), "filter.evil.clean=") == 1);
	// The listing itself runs with the safe base and nothing of the repository's.
	const auto& listing = calls[calls.size() - 2];
	BOOST_TEST(has_arg(listing, "--get-regexp"));
	BOOST_TEST(has_arg(listing, "core.fsmonitor=false"));
}

BOOST_AUTO_TEST_CASE(AFilterDriverThatCannotBeOverridden_RefusesTheRead)
{
	filter_config = "filter.a=b.clean\nx\0"s;
	bool ran = false;
	command = [&ran](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { ran = true; return exited(0); };
	make_service(fake_options());

	const json::value result = finish(service->start_status());
	BOOST_TEST(text(result.as_object().at("error").as_object().at("code")) == "unsafe_repository");
	BOOST_TEST(!ran);
}

BOOST_AUTO_TEST_CASE(TheEnvironment_LosesWhatRedirectsGitOrAddsConfig)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	auto options = fake_options();
	for (const char* entry : { "GIT_DIR=/elsewhere/.git", "GIT_WORK_TREE=/elsewhere", "GIT_INDEX_FILE=/i",
			"GIT_OBJECT_DIRECTORY=/o", "GIT_ALTERNATE_OBJECT_DIRECTORIES=/a", "GIT_CONFIG_PARAMETERS='core.fsmonitor'='x'",
			"GIT_CONFIG_COUNT=1", "GIT_CONFIG_KEY_0=core.fsmonitor", "GIT_CONFIG_VALUE_0=x", "GIT_EXEC_PATH=/evil",
			"GIT_EXTERNAL_DIFF=/evil", "GIT_CEILING_DIRECTORIES=/kept", "HOME=/kept" }) {
		options.environment->emplace_back(entry);
	}
	make_service(std::move(options));
	finish(service->start_status());

	const auto& env = *last_options.environment;
	for (const char* gone : { "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
			"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0",
			"GIT_CONFIG_VALUE_0", "GIT_EXEC_PATH", "GIT_EXTERNAL_DIFF" }) {
		BOOST_TEST_CONTEXT(gone) {
			BOOST_TEST(!edt::variable_of(env, gone).has_value());
		}
	}
	BOOST_TEST(edt::variable_of(env, "GIT_CEILING_DIRECTORIES").value_or("") == "/kept");
	BOOST_TEST(edt::variable_of(env, "HOME").value_or("") == "/kept");

	auto empty_exec = fake_options();
	empty_exec.environment->emplace_back("GIT_EXEC_PATH=");
	make_service(std::move(empty_exec));
	finish(service->start_status());
	BOOST_TEST(edt::variable_of(*last_options.environment, "GIT_EXEC_PATH").has_value());
}

// The watchdog stopped nothing: git answered (the runner ignored the stop), so the probe is not a hang.
BOOST_AUTO_TEST_CASE(AProbeThatFinishesPastTheDeadline_WithoutBeingStopped_IsNotAHang)
{
	auto options = fake_options();
	options.probe_timeout = std::chrono::milliseconds(20);
	options.run = [this](const fs::path&, const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "--version")) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			on_line("git version 2.45.1\n");
			return exited(0);
		}
		on_line(edt::utf8_of(base) + "\n");
		return exited(0);
	};
	make_service(std::move(options));
	BOOST_TEST(service->info().available, service->info().reason);
	BOOST_TEST(service->info().git_version == "2.45.1");
}

// A job.cancel while git runs reaches the runner's stop token; the job ends cancelled.
BOOST_AUTO_TEST_CASE(Cancel_StopsTheRunningGit)
{
	edt::job_service::job_id running = 0;
	bool saw_stop = false;
	command = [this, &running, &saw_stop](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token stop) {
		jobs->cancel(running);
		saw_stop = stop.stop_requested();
		platform::process_result result = exited(-1);
		result.cancelled = saw_stop;
		return result;
	};
	make_service(fake_options());
	const auto started = service->start_status();
	BOOST_REQUIRE(static_cast<bool>(started));
	running = started.job;
	run_to_end();

	BOOST_TEST(saw_stop);
	const auto info = jobs->info(running);
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST((info->status == edt::job_status::cancelled));
	BOOST_TEST(!info->result.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
