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
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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

// Forward slashes: git (for Windows too) hands these paths to its sh.
std::string generic_text(const fs::path& path)
{
	const std::u8string utf8 = path.generic_u8string();
	return std::string(utf8.begin(), utf8.end());
}

// A script git runs through sh; written with \n line ends, executable where that is a permission.
void make_script(const fs::path& file, std::string_view body)
{
	fs::create_directories(file.parent_path());
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		const std::string text = "#!/bin/sh\n" + std::string(body);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
	}
#ifndef _WIN32
	fs::permissions(file, fs::perms::owner_all, fs::perm_options::add);
#endif
}

bool is_error(const json::value& result, std::string_view code)
{
	const json::object& object = result.as_object();
	return object.contains("error") && object.at("error").as_object().at("code").as_string() == code;
}

std::string arg_starting_with(const std::vector<std::string>& args, std::string_view prefix)
{
	const auto found = std::find_if(args.begin(), args.end(), [prefix](const std::string& a) { return a.starts_with(prefix); });
	return found == args.end() ? std::string{} : *found;
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

	// What git prints on stdout, as it wrote it.
	std::string git_text(const fs::path& dir, const std::vector<std::string>& args)
	{
		std::string output;
		platform::process_options options;
		options.working_dir = dir;
		options.environment = environment;
		options.raw_output = true;
		const auto result = platform::run_process(real_git(), args, options,
			[&output](std::string_view chunk) { output.append(chunk); }, {});
		BOOST_REQUIRE_MESSAGE(result.error.empty() && result.exit_code == 0,
			std::format("git {}: {} (exit {})", args.empty() ? std::string{} : args.front(), result.error, result.exit_code));
		return output;
	}

	std::string head_of(const fs::path& dir)
	{
		std::string text = git_text(dir, { "rev-parse", "HEAD" });
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
		return text;
	}

	// Only the repository's own config counts: a system or global one (git for Windows sets filter.lfs in its
	// system config) stays out of the tests that depend on what is configured.
	void isolate_config()
	{
		const fs::path global = base / "global.gitconfig";
		write(global, "");
		edt::set_variable(environment, "GIT_CONFIG_NOSYSTEM", "1");
		edt::set_variable(environment, "GIT_CONFIG_GLOBAL", edt::utf8_of(global));
	}

	// <base>/<name>: one commit of a.txt on main.
	fs::path make_plain_repository(std::string_view name)
	{
		const fs::path repo = base / name;
		fs::create_directories(repo);
		git(repo, { "init", "-q" });
		git(repo, { "symbolic-ref", "HEAD", "refs/heads/main" });
		git(repo, { "config", "user.name", "Test User" });
		git(repo, { "config", "user.email", "test@example.com" });
		git(repo, { "config", "core.autocrlf", "false" });
		write(repo / "a.txt", "one\n");
		git(repo, { "add", "a.txt" });
		git(repo, { "commit", "-q", "-m", "first commit" });
		return repo;
	}

	// <base>/<name>: a bare repository whose HEAD is main.
	fs::path make_bare(std::string_view name)
	{
		const fs::path remote = base / name;
		fs::create_directories(remote);
		git(remote, { "init", "-q", "--bare" });
		git(remote, { "symbolic-ref", "HEAD", "refs/heads/main" });
		return remote;
	}

	static std::string file_url(const fs::path& path)
	{
		const std::u8string utf8 = path.generic_u8string();
		const std::string generic(utf8.begin(), utf8.end());
		return "file://" + (generic.starts_with('/') ? generic : "/" + generic);
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
			if (has_arg(args, "--git-path")) {
				on_line(edt::utf8_of(base / "FETCH_HEAD") + "\n");
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
	BOOST_TEST(registrations.size() == 13u);

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

// stage, unstage, commit (a multi-line UTF-8 message, through its file), branch.create and checkout on a real
// repository, read back through the service's reads.
BOOST_AUTO_TEST_CASE(RealRepository_StageUnstageCommitBranchCheckout)
{
	isolate_config();
	const fs::path repo = make_repository();
	make_service(real_options(repo));
	constexpr auto person = cmd::origin_kind::ui;
	const auto entry_of = [](const json::value& status, std::string_view path) -> const json::object* {
		for (const auto& item : status.as_object().at("entries").as_array()) {
			if (item.as_object().at("path").as_string() == path) return &item.as_object();
		}
		return nullptr;
	};

	write(repo / "fresh file.txt", "fresh\n");
	const json::value staged = finish(service->start_stage({ "fresh file.txt", "a.txt" }, person));
	BOOST_TEST(!staged.as_object().contains("error"), json::serialize(staged));
	const json::value after_stage = finish(service->start_status());
	const json::object* fresh = entry_of(after_stage, "fresh file.txt");
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(text(fresh->at("index")) == "A");

	const json::value unstaged = finish(service->start_unstage({ "fresh file.txt" }, person));
	BOOST_TEST(!unstaged.as_object().contains("error"), json::serialize(unstaged));
	const json::value after_unstage = finish(service->start_status());
	fresh = entry_of(after_unstage, "fresh file.txt");
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(text(fresh->at("kind")) == "untracked");
	BOOST_TEST(fs::exists(repo / "fresh file.txt"));

	const std::string message = "Subject with " + u_umlaut + "\n\nBody line one\nBody line two \xE2\x9C\x93\n";
	const json::value committed = finish(service->start_commit(message, person));
	BOOST_TEST_INFO("commit: " << json::serialize(committed));
	BOOST_REQUIRE(committed.as_object().contains("hash"));
	BOOST_TEST(text(committed.as_object().at("hash")) == head_of(repo));
	std::string body = git_text(repo, { "log", "-1", "--format=%B" });
	while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
	BOOST_TEST(body == message.substr(0, message.size() - 1));
	const json::value log = finish(service->start_log({}, {}, 1, {}));
	BOOST_TEST(text(log.as_object().at("commits").as_array()[0].as_object().at("subject")) == "Subject with " + u_umlaut);
	const json::value after_commit = finish(service->start_status());
	BOOST_TEST(entry_of(after_commit, "a.txt") == nullptr);

	BOOST_TEST(service->start_branch_create("-x", {}, person).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_branch_create("ok", "--orphan", person).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_checkout("--force", person).code == cmd::codes::invalid_args);
	const json::value created = finish(service->start_branch_create("topic", "main", person));
	BOOST_TEST(!created.as_object().contains("error"), json::serialize(created));
	const json::value checked_out = finish(service->start_checkout("topic", person));
	BOOST_TEST(!checked_out.as_object().contains("error"), json::serialize(checked_out));
	const json::value on_topic = finish(service->start_status());
	BOOST_TEST(text(on_topic.as_object().at("branch").as_object().at("head")) == "topic");
	finish(service->start_checkout("main", person));
	const json::value branches = finish(service->start_branches());
	BOOST_TEST(text(branches.as_object().at("current")) == "main");

	// Git's own refusal is the job's result: nothing to commit.
	const json::value nothing = finish(service->start_commit("nothing staged", person));
	BOOST_TEST(nothing.as_object().contains("error"), json::serialize(nothing));
	BOOST_TEST(head_of(repo) == text(committed.as_object().at("hash")));
}

BOOST_AUTO_TEST_CASE(RealRepository_FetchPullPush_AgainstALocalBareRepository)
{
	isolate_config();
	const fs::path remote = base / "remote.git";
	fs::create_directories(remote);
	git(remote, { "init", "-q", "--bare" });
	git(remote, { "symbolic-ref", "HEAD", "refs/heads/main" });
	const std::string remote_path = generic_text(remote);
	const std::string url = "file://" + (remote_path.starts_with('/') ? remote_path : "/" + remote_path);
	const fs::path repo = make_plain_repository("local");
	git(repo, { "remote", "add", "origin", url });
	make_service(real_options(repo));
	constexpr auto person = cmd::origin_kind::ui;

	for (const char* bad : { "+main", "-x", "a:b", "main:refs/heads/other" }) {
		BOOST_TEST_CONTEXT(bad) {
			BOOST_TEST(service->start_push("origin", bad, false, person).code == cmd::codes::invalid_args);
		}
	}
	BOOST_TEST(service->start_push("file:///elsewhere", "main", false, person).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_push("--mirror", "main", false, person).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_pull({}, "main", person).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_pull("origin", "+main", person).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_fetch("-q", person).code == cmd::codes::invalid_args);
	BOOST_TEST(jobs->list().empty());

	const json::value pushed = finish(service->start_push("origin", "main", true, person));
	BOOST_TEST(!pushed.as_object().contains("error"), json::serialize(pushed));
	BOOST_TEST(head_of(remote) == head_of(repo));

	// Someone else pushes a commit on top.
	const fs::path other = base / "other";
	git(base, { "clone", "-q", url, edt::utf8_of(other) });
	git(other, { "config", "user.name", "Other User" });
	git(other, { "config", "user.email", "other@example.com" });
	git(other, { "config", "commit.gpgsign", "false" });
	write(other / "theirs.txt", "theirs\n");
	git(other, { "add", "theirs.txt" });
	git(other, { "commit", "-q", "-m", "their commit" });
	git(other, { "push", "-q", "origin", "main" });
	const std::string theirs = head_of(other);
	BOOST_TEST(head_of(repo) != theirs);

	const json::value fetched = finish(service->start_fetch("origin", person));
	BOOST_TEST(!fetched.as_object().contains("error"), json::serialize(fetched));
	std::string tracking = git_text(repo, { "rev-parse", "origin/main" });
	while (!tracking.empty() && (tracking.back() == '\n' || tracking.back() == '\r')) tracking.pop_back();
	BOOST_TEST(tracking == theirs);
	BOOST_TEST(head_of(repo) != theirs);

	// The upstream set by the push: a pull without arguments fast-forwards to it.
	const json::value pulled = finish(service->start_pull({}, {}, person));
	BOOST_TEST(!pulled.as_object().contains("error"), json::serialize(pulled));
	BOOST_TEST(head_of(repo) == theirs);
	BOOST_TEST(fs::exists(repo / "theirs.txt"));

	write(other / "more.txt", "more\n");
	git(other, { "add", "more.txt" });
	git(other, { "commit", "-q", "-m", "one more" });
	git(other, { "push", "-q", "origin", "main" });
	const json::value named = finish(service->start_pull("origin", "main", person));
	BOOST_TEST(!named.as_object().contains("error"), json::serialize(named));
	BOOST_TEST(head_of(repo) == head_of(other));

	// Diverged: a pull that is not a fast-forward is refused, nothing is merged.
	write(other / "diverge.txt", "theirs\n");
	git(other, { "add", "diverge.txt" });
	git(other, { "commit", "-q", "-m", "their side" });
	git(other, { "push", "-q", "origin", "main" });
	write(repo / "mine.txt", "mine\n");
	finish(service->start_stage({ "mine.txt" }, person));
	const std::string mine = text(finish(service->start_commit("my side", person)).as_object().at("hash"));
	const json::value refused = finish(service->start_pull({}, {}, person));
	BOOST_TEST(refused.as_object().contains("error"), json::serialize(refused));
	BOOST_TEST(head_of(repo) == mine);
	// And a push that is not a fast-forward is rejected, never forced.
	const json::value rejected = finish(service->start_push("origin", "main", false, person));
	BOOST_TEST(rejected.as_object().contains("error"), json::serialize(rejected));
	BOOST_TEST(head_of(remote) == head_of(other));
}

// Owner decision (#454): an agent's commit runs no hook of the repository; a person's does.
BOOST_AUTO_TEST_CASE(AHostilePreCommitHook_RunsForAPersonButNotForAnAgent)
{
	isolate_config();
	const fs::path repo = make_plain_repository("hooks");
	const fs::path marker = base / "hook_ran.txt";
	make_script(repo / ".git" / "hooks" / "pre-commit", "echo ran >> '" + generic_text(marker) + "'\nexit 0\n");
	make_service(real_options(repo));
	std::error_code ec;

	write(repo / "a.txt", "by an agent\n");
	const json::value staged = finish(service->start_stage({ "a.txt" }, cmd::origin_kind::agent));
	BOOST_TEST(!staged.as_object().contains("error"), json::serialize(staged));
	const json::value by_agent = finish(service->start_commit("by an agent", cmd::origin_kind::agent));
	BOOST_REQUIRE_MESSAGE(by_agent.as_object().contains("hash"), json::serialize(by_agent));
	BOOST_TEST(head_of(repo) == text(by_agent.as_object().at("hash")));
	BOOST_TEST(!fs::exists(marker, ec), "an agent's commit ran the repository's pre-commit hook");

	write(repo / "a.txt", "by a person\n");
	finish(service->start_stage({ "a.txt" }, cmd::origin_kind::ui));
	const json::value by_person = finish(service->start_commit("by a person", cmd::origin_kind::ui));
	BOOST_REQUIRE_MESSAGE(by_person.as_object().contains("hash"), json::serialize(by_person));
	BOOST_TEST(fs::exists(marker, ec), "a person's commit must run the hooks as plain git does");
}

// Owner decision (#454): a credential helper from the repository's own config refuses an agent's network call before
// git runs; a person's push goes on, and so does an agent's with allow_repo_credential_helper.
BOOST_AUTO_TEST_CASE(ARepositoryCredentialHelper_RefusesAnAgentsPush)
{
	isolate_config();
	const fs::path remote = base / "helper_remote.git";
	fs::create_directories(remote);
	git(remote, { "init", "-q", "--bare" });
	git(remote, { "symbolic-ref", "HEAD", "refs/heads/main" });
	const std::string remote_path = generic_text(remote);
	const std::string url = "file://" + (remote_path.starts_with('/') ? remote_path : "/" + remote_path);
	const fs::path repo = make_plain_repository("helper");
	git(repo, { "remote", "add", "origin", url });
	const fs::path marker = base / "helper_ran.txt";
	const fs::path script = base / "helper.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\n");
	git(repo, { "config", "credential.helper", generic_text(script) });
	make_service(real_options(repo));
	std::error_code ec;

	const json::value refused = finish(service->start_push("origin", "main", false, cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("a credential helper set by the repository")
		!= std::string::npos);
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(git_text(remote, { "for-each-ref" }).empty());
	BOOST_TEST(is_error(finish(service->start_fetch("origin", cmd::origin_kind::agent)), "unsafe_repository"));
	BOOST_TEST(is_error(finish(service->start_pull({}, {}, cmd::origin_kind::agent)), "unsafe_repository"));

	// With the opt-in the helper rule lets the push through to git, which then refuses the file transport: a guarded
	// call reaches only https and ssh remotes.
	service->set_write_permissions({ false, false, true, false });
	const json::value allowed = finish(service->start_push("origin", "main", false, cmd::origin_kind::agent));
	BOOST_TEST_INFO("allowed: " << json::serialize(allowed));
	BOOST_TEST(!is_error(allowed, "unsafe_repository"));
	BOOST_TEST(allowed.as_object().contains("error"));
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(git_text(remote, { "for-each-ref" }).empty());

	const json::value by_person = finish(service->start_push("origin", "main", false, cmd::origin_kind::ui));
	BOOST_TEST(!by_person.as_object().contains("error"), json::serialize(by_person));
	BOOST_TEST(head_of(remote) == head_of(repo));
}

// A helper set in a file the local config includes is the repository's too (git reports the including file's scope).
BOOST_AUTO_TEST_CASE(ACredentialHelperFromAnIncludeOfTheLocalConfig_RefusesAnAgentsPush)
{
	isolate_config();
	const fs::path remote = base / "include_remote.git";
	fs::create_directories(remote);
	git(remote, { "init", "-q", "--bare" });
	const std::string remote_path = generic_text(remote);
	const std::string url = "file://" + (remote_path.starts_with('/') ? remote_path : "/" + remote_path);
	const fs::path repo = make_plain_repository("include");
	git(repo, { "remote", "add", "origin", url });
	const fs::path marker = base / "include_helper_ran.txt";
	const fs::path script = base / "include_helper.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\n");
	const fs::path included = base / "included.gitconfig";
	write(included, "[credential]\n\thelper = " + generic_text(script) + "\n");
	git(repo, { "config", "include.path", generic_text(included) });
	make_service(real_options(repo));

	const json::value refused = finish(service->start_push("origin", "main", false, cmd::origin_kind::agent));
	BOOST_TEST(is_error(refused, "unsafe_repository"), json::serialize(refused));
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(git_text(remote, { "for-each-ref" }).empty());
}

// git checkout <name> of a branch only a remote has: the filter check diffs against that remote's branch, and git
// creates the local tracking branch.
BOOST_AUTO_TEST_CASE(AnAgentsCheckoutOfARemoteOnlyBranch_IsCheckedAgainstTheRemoteBranch)
{
	isolate_config();
	const fs::path remote = base / "dwim_remote.git";
	fs::create_directories(remote);
	git(remote, { "init", "-q", "--bare" });
	git(remote, { "symbolic-ref", "HEAD", "refs/heads/main" });
	const std::string remote_path = generic_text(remote);
	const std::string url = "file://" + (remote_path.starts_with('/') ? remote_path : "/" + remote_path);
	const fs::path repo = make_plain_repository("dwim");
	git(repo, { "remote", "add", "origin", url });
	git(repo, { "push", "-q", "origin", "main" });

	const fs::path other = base / "dwim_other";
	git(base, { "clone", "-q", url, edt::utf8_of(other) });
	git(other, { "config", "user.name", "Other User" });
	git(other, { "config", "user.email", "other@example.com" });
	git(other, { "config", "commit.gpgsign", "false" });
	git(other, { "checkout", "-q", "-b", "feature" });
	write(other / "notes.txt", "notes\n");
	git(other, { "add", "notes.txt" });
	git(other, { "commit", "-q", "-m", "notes" });
	git(other, { "push", "-q", "origin", "feature" });
	git(other, { "checkout", "-q", "-b", "feature_bin", "main" });
	write(other / "data.bin", "raw bytes\n");
	git(other, { "add", "data.bin" });
	git(other, { "commit", "-q", "-m", "data" });
	git(other, { "push", "-q", "origin", "feature_bin" });
	git(repo, { "fetch", "-q", "origin" });

	// A filter driver no attribute names.
	const fs::path marker = base / "dwim_filter_ran.txt";
	const fs::path script = base / "dwim-filter.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\ncat\n");
	git(repo, { "config", "filter.lfs.clean", generic_text(script) });
	git(repo, { "config", "filter.lfs.smudge", generic_text(script) });
	make_service(real_options(repo));
	const auto agent = cmd::origin_kind::agent;

	const json::value checked_out = finish(service->start_checkout("feature", agent));
	BOOST_TEST(!checked_out.as_object().contains("error"), json::serialize(checked_out));
	std::string upstream = git_text(repo, { "rev-parse", "--abbrev-ref", "feature@{upstream}" });
	while (!upstream.empty() && (upstream.back() == '\n' || upstream.back() == '\r')) upstream.pop_back();
	BOOST_TEST(upstream == "origin/feature");
	BOOST_TEST(fs::exists(repo / "notes.txt"));
	const json::value back = finish(service->start_checkout("main", agent));
	BOOST_TEST(!back.as_object().contains("error"), json::serialize(back));

	// Now an attribute names the driver for a file the remote's branch adds (not a .gitattributes the checkout changes).
	write(repo / ".git" / "info" / "attributes", "*.bin filter=lfs\n");
	const json::value refused = finish(service->start_checkout("feature_bin", agent));
	BOOST_TEST(is_error(refused, "filters_require_opt_in"), json::serialize(refused));
	BOOST_TEST(git_text(repo, { "for-each-ref", "refs/heads/feature_bin" }).empty());
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
}

// A name that resolves to a commit (here a tag) is what git checks out, even when a remote has a branch of that name:
// the filter check diffs against the tag, not the remote branch.
BOOST_AUTO_TEST_CASE(AnAgentsCheckoutOfATagNamedLikeARemoteBranch_IsCheckedAgainstTheTag)
{
	isolate_config();
	const fs::path remote = base / "tag_remote.git";
	fs::create_directories(remote);
	git(remote, { "init", "-q", "--bare" });
	git(remote, { "symbolic-ref", "HEAD", "refs/heads/main" });
	const std::string remote_path = generic_text(remote);
	const std::string url = "file://" + (remote_path.starts_with('/') ? remote_path : "/" + remote_path);
	const fs::path repo = make_plain_repository("tagged");
	git(repo, { "remote", "add", "origin", url });
	git(repo, { "push", "-q", "origin", "main" });
	// origin/feature: notes only.
	git(repo, { "checkout", "-q", "-b", "remote_feature" });
	write(repo / "notes.txt", "notes\n");
	git(repo, { "add", "notes.txt" });
	git(repo, { "commit", "-q", "-m", "notes" });
	git(repo, { "push", "-q", "origin", "remote_feature:feature" });
	// Tag feature: a commit that adds data.bin.
	git(repo, { "checkout", "-q", "-b", "side", "main" });
	write(repo / "data.bin", "raw bytes\n");
	git(repo, { "add", "data.bin" });
	git(repo, { "commit", "-q", "-m", "data" });
	git(repo, { "tag", "feature" });
	git(repo, { "checkout", "-q", "main" });
	git(repo, { "fetch", "-q", "origin" });

	const fs::path script = base / "tag-filter.sh";
	make_script(script, "cat\n");
	git(repo, { "config", "filter.lfs.clean", generic_text(script) });
	git(repo, { "config", "filter.lfs.smudge", generic_text(script) });
	write(repo / ".git" / "info" / "attributes", "*.bin filter=lfs\n");
	const std::string main_head = head_of(repo);
	make_service(real_options(repo));

	const json::value refused = finish(service->start_checkout("feature", cmd::origin_kind::agent));
	BOOST_TEST(is_error(refused, "filters_require_opt_in"), json::serialize(refused));
	BOOST_TEST(head_of(repo) == main_head);
	BOOST_TEST(git_text(repo, { "for-each-ref", "refs/heads/feature" }).empty());
}

// Owner decision (#454): a configured filter driver that applies to a file of the commit refuses an agent's commit
// before git runs (switching the driver off would commit LFS files as raw bytes); allow_filters lets it run, and the
// filter runs. A driver no attribute names (a global filter.lfs the repository does not use; the repository's config
// stands in for the global one here) blocks nothing.
BOOST_AUTO_TEST_CASE(AnLfsStyleFilter_RefusesAnAgentsCommit_UnlessFiltersAreAllowed)
{
	isolate_config();
	const fs::path repo = make_plain_repository("lfs");
	const fs::path marker = base / "filter_ran.txt";
	const fs::path script = base / "lfs-filter.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\ncat\n");
	git(repo, { "config", "filter.lfs.clean", generic_text(script) });
	git(repo, { "config", "filter.lfs.smudge", generic_text(script) });
	std::error_code ec;
	{
		make_service(real_options(repo));
		write(repo / "plain.bin", "not under a filter yet\n");
		const json::value staged = finish(service->start_stage({ "plain.bin" }, cmd::origin_kind::agent));
		BOOST_TEST(!staged.as_object().contains("error"), json::serialize(staged));
		const json::value committed = finish(service->start_commit("no attribute names the filter", cmd::origin_kind::agent));
		BOOST_REQUIRE_MESSAGE(committed.as_object().contains("hash"), json::serialize(committed));
		BOOST_TEST(!fs::exists(marker, ec));
	}

	write(repo / ".gitattributes", "*.bin filter=lfs\n");
	write(repo / "data.bin", "raw bytes\n");
	// A person stages with the filter, as plain git does.
	git(repo, { "add", ".gitattributes", "data.bin" });
	const bool filters_run_here = fs::exists(marker, ec);
#ifdef _WIN32
	BOOST_WARN_MESSAGE(filters_run_here, "plain git ran no filter here: the last check below is skipped");
#else
	BOOST_REQUIRE_MESSAGE(filters_run_here, "plain git must run the filter, or this test proves nothing");
#endif
	fs::remove(marker, ec);
	const std::string before = head_of(repo);
	make_service(real_options(repo));

	const json::value refused = finish(service->start_commit("agent commit", cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "filters_require_opt_in"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("lfs") != std::string::npos);
	BOOST_TEST(head_of(repo) == before);
	write(repo / "other.bin", "more raw bytes\n");
	BOOST_TEST(is_error(finish(service->start_stage({ "other.bin" }, cmd::origin_kind::agent)), "filters_require_opt_in"));
	BOOST_TEST(!fs::exists(marker, ec), "a refused write ran the filter");

	service->set_write_permissions({ false, true, false, false });
	const json::value staged = finish(service->start_stage({ "other.bin" }, cmd::origin_kind::agent));
	BOOST_TEST(!staged.as_object().contains("error"), json::serialize(staged));
	const json::value committed = finish(service->start_commit("agent commit", cmd::origin_kind::agent));
	BOOST_REQUIRE_MESSAGE(committed.as_object().contains("hash"), json::serialize(committed));
	BOOST_TEST(head_of(repo) != before);
	BOOST_TEST(head_of(repo) == text(committed.as_object().at("hash")));
	if (filters_run_here) BOOST_TEST(fs::exists(marker, ec), "with allow_filters the filter must run");
}

// The writes are mutating commands with no agent category (closed to agents until #455).
BOOST_AUTO_TEST_CASE(WriteCommands_MutateAndHaveNoAgentCategory)
{
	isolate_config();
	const fs::path repo = make_plain_repository("commands");
	make_service(real_options(repo));
	registrations = edt::register_vcs_commands(commands, *service);
	for (const char* id : { "vcs.stage", "vcs.unstage", "vcs.commit", "vcs.branch.create", "vcs.checkout", "vcs.fetch",
			"vcs.pull", "vcs.push" }) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* desc = commands.find(id);
			BOOST_REQUIRE(desc != nullptr);
			BOOST_TEST(desc->mutates);
			BOOST_TEST((desc->category == cmd::agent_category::unset));
		}
	}
	for (const char* id : { "vcs.reset", "vcs.rebase", "vcs.branch.delete", "vcs.push.force" }) {
		BOOST_TEST(commands.find(id) == nullptr, id);
	}

	const auto invalid = [this](std::string_view id, std::string_view args) {
		return commands.invoke(id, json::parse(args), cmd::origin::test()).code == cmd::codes::invalid_args;
	};
	BOOST_TEST(invalid("vcs.stage", R"({"paths":[]})"));
	BOOST_TEST(invalid("vcs.stage", R"({"paths":[""]})"));
	BOOST_TEST(invalid("vcs.commit", R"({"message":""})"));
	BOOST_TEST(invalid("vcs.commit", R"({"message":"  \n"})"));
	BOOST_TEST(invalid("vcs.push", R"({"remote":"origin","branch":"+main"})"));
	BOOST_TEST(invalid("vcs.push", R"({"remote":"origin","branch":"-f"})"));
	BOOST_TEST(invalid("vcs.push", R"({"remote":"origin","branch":"a:b"})"));
	BOOST_TEST(invalid("vcs.push", R"({"remote":"origin","branch":"main","force":true})"));
	BOOST_TEST(invalid("vcs.pull", R"({"branch":"main"})"));
	BOOST_TEST(invalid("vcs.checkout", R"({"ref":"-f"})"));
	BOOST_TEST(jobs->list().empty());

	write(repo / "b.txt", "bee\n");
	const cmd::result stage = commands.invoke("vcs.stage", json::parse(R"({"paths":["b.txt"]})"), cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(stage.ok, stage.error);
	finish(static_cast<edt::job_service::job_id>(stage.value.as_object().at("job").as_int64()));
	const cmd::result commit = commands.invoke("vcs.commit", json::parse(R"({"message":"b"})"), cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(commit.ok, commit.error);
	const json::value result = finish(static_cast<edt::job_service::job_id>(commit.value.as_object().at("job").as_int64()));
	BOOST_TEST(text(result.as_object().at("hash")) == head_of(repo));
}

// Security review of #474: reset, commit, checkout and merge re-hash a tracked file whose stat is stale through its
// clean filter, whatever paths the call names. A guarded write refuses while any file of the index is under a
// configured driver; with allow_any_repo_config too (filter.* passes the allowlist anyway).
BOOST_AUTO_TEST_CASE(AGuardedWrite_RefusesAFilterOnAStaleTrackedFile)
{
	isolate_config();
	const fs::path remote = make_bare("stale_remote.git");
	const fs::path repo = make_plain_repository("stale");
	write(repo / ".gitattributes", "*.dat filter=evil\n");
	write(repo / "a.dat", "data\n");
	git(repo, { "add", ".gitattributes", "a.dat" });
	git(repo, { "commit", "-q", "-m", "attributes" });
	git(repo, { "branch", "other" });
	git(repo, { "remote", "add", "origin", file_url(remote) });
	git(repo, { "push", "-q", "-u", "origin", "main" });
	write(repo / "t.txt", "t\n");
	git(repo, { "add", "t.txt" });
	const fs::path marker = base / "stale_filter_ran.txt";
	const fs::path script = base / "stale-filter.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\ncat\n");
	git(repo, { "config", "filter.evil.clean", generic_text(script) });
	git(repo, { "config", "filter.evil.smudge", generic_text(script) });
	// Same bytes, new stat: what unzipping does to every file.
	write(repo / "a.dat", "data\n");
	const std::string before = head_of(repo);
	make_service(real_options(repo));
	std::error_code ec;

	for (const bool any_config : { false, true }) {
		BOOST_TEST_CONTEXT("allow_any_repo_config " << any_config) {
			service->set_write_permissions({ false, false, false, any_config });
			const auto agent = cmd::origin_kind::agent;
			BOOST_TEST(is_error(finish(service->start_unstage({ "t.txt" }, agent)), "filters_require_opt_in"));
			BOOST_TEST(is_error(finish(service->start_commit("message", agent)), "filters_require_opt_in"));
			BOOST_TEST(is_error(finish(service->start_checkout("other", agent)), "filters_require_opt_in"));
			// Pull: its fetch is refused first (file transport); either way the merge never runs.
			BOOST_TEST(finish(service->start_pull({}, {}, agent)).as_object().contains("error"));
			BOOST_TEST(head_of(repo) == before);
			BOOST_TEST(!fs::exists(marker, ec), "a guarded write ran the clean filter of a stale tracked file");
		}
	}
}

// Security review of #474: with branch x and tag x, `git diff HEAD x` takes the tag while `git checkout x` takes the
// branch. The guarded checkout resolves once (the branch first) and checks the tree it checks out.
BOOST_AUTO_TEST_CASE(AGuardedCheckout_OfANameThatIsABranchAndATag_ChecksTheBranch)
{
	isolate_config();
	const fs::path repo = make_plain_repository("ambiguous");
	git(repo, { "checkout", "-q", "-b", "x" });
	write(repo / "notes.txt", "notes\n");
	git(repo, { "add", "notes.txt" });
	git(repo, { "commit", "-q", "-m", "notes" });
	git(repo, { "checkout", "-q", "-b", "side", "main" });
	write(repo / "data.bin", "raw bytes\n");
	git(repo, { "add", "data.bin" });
	git(repo, { "commit", "-q", "-m", "data" });
	git(repo, { "tag", "x" });
	git(repo, { "checkout", "-q", "main" });
	const fs::path marker = base / "ambiguous_filter_ran.txt";
	const fs::path script = base / "ambiguous-filter.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\ncat\n");
	git(repo, { "config", "filter.lfs.clean", generic_text(script) });
	git(repo, { "config", "filter.lfs.smudge", generic_text(script) });
	write(repo / ".git" / "info" / "attributes", "*.bin filter=lfs\n");
	make_service(real_options(repo));
	service->set_write_permissions({ false, false, false, true });

	const json::value checked_out = finish(service->start_checkout("x", cmd::origin_kind::agent));
	BOOST_TEST(!checked_out.as_object().contains("error"), json::serialize(checked_out));
	std::string head_ref = git_text(repo, { "symbolic-ref", "HEAD" });
	while (!head_ref.empty() && (head_ref.back() == '\n' || head_ref.back() == '\r')) head_ref.pop_back();
	BOOST_TEST(head_ref == "refs/heads/x");
	BOOST_TEST(fs::exists(repo / "notes.txt"));
	std::error_code ec;
	BOOST_TEST(!fs::exists(repo / "data.bin", ec));
	BOOST_TEST(!fs::exists(marker, ec));
}

// Security review of #474: a push over the file transport runs the other repository's hooks, without the -c
// overrides (git drops them for the local transport). A guarded call allows only https and ssh.
BOOST_AUTO_TEST_CASE(AGuardedPushToALocalRepository_IsRefusedByGit_AndRunsNoHookThere)
{
	isolate_config();
	const fs::path remote = make_bare("hooked_remote.git");
	const fs::path marker = base / "pre_receive_ran.txt";
	make_script(remote / "hooks" / "pre-receive", "echo ran >> '" + generic_text(marker) + "'\nexit 0\n");
	const fs::path repo = make_plain_repository("hooked");
	git(repo, { "remote", "add", "origin", file_url(remote) });
	make_service(real_options(repo));
	std::error_code ec;

	for (const bool any_config : { false, true }) {
		BOOST_TEST_CONTEXT("allow_any_repo_config " << any_config) {
			service->set_write_permissions({ false, false, false, any_config });
			const json::value refused = finish(service->start_push("origin", "main", false, cmd::origin_kind::agent));
			BOOST_TEST_INFO("refused: " << json::serialize(refused));
			BOOST_TEST(refused.as_object().contains("error"));
			BOOST_TEST(!is_error(refused, "unsafe_repository"));
			BOOST_TEST(text(refused.as_object().at("error").as_object().at("stderr_tail")).find("not allowed") != std::string::npos);
			BOOST_TEST(!fs::exists(marker, ec));
			BOOST_TEST(git_text(remote, { "for-each-ref" }).empty());
		}
	}

	// A person's push goes through and the hook runs: the hook above is live.
	const json::value by_person = finish(service->start_push("origin", "main", false, cmd::origin_kind::ui));
	BOOST_TEST(!by_person.as_object().contains("error"), json::serialize(by_person));
#ifdef _WIN32
	BOOST_WARN_MESSAGE(fs::exists(marker, ec), "the pre-receive hook did not run here: the checks above prove less");
#else
	BOOST_TEST(fs::exists(marker, ec), "the pre-receive hook must run for a person, or this test proves nothing");
#endif
}

// Security review of #474: core.alternateRefsCommand runs on a fetch when the repository has alternates. The
// allowlist refuses the key; with allow_any_repo_config the override (and the transport rule) still keep it from
// running.
BOOST_AUTO_TEST_CASE(AlternateRefsCommand_RunsNotOnAGuardedFetch)
{
	isolate_config();
	const fs::path remote = make_bare("alternates_remote.git");
	const fs::path source = make_plain_repository("alternates_source");
	git(source, { "push", "-q", file_url(remote), "main" });
	const fs::path repo = make_plain_repository("alternates");
	write(repo / ".git" / "objects" / "info" / "alternates", generic_text(source / ".git" / "objects") + "\n");
	git(repo, { "remote", "add", "origin", file_url(remote) });
	const fs::path marker = base / "alternate_refs_ran.txt";
	const fs::path script = base / "alternate-refs.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\n");
	git(repo, { "config", "core.alternateRefsCommand", generic_text(script) });
	make_service(real_options(repo));
	std::error_code ec;

	const json::value refused = finish(service->start_fetch("origin", cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("core.alternaterefscommand") != std::string::npos);
	BOOST_TEST(!fs::exists(marker, ec));

	service->set_write_permissions({ false, false, false, true });
	const json::value overridden = finish(service->start_fetch("origin", cmd::origin_kind::agent));
	BOOST_TEST(!is_error(overridden, "unsafe_repository"), json::serialize(overridden));
	BOOST_TEST(!fs::exists(marker, ec));
}

// Security review of #474: merge.verifySignatures makes pull's merge run gpg.ssh.program. The allowlist refuses the
// keys; with allow_any_repo_config the overrides still keep the program from running.
BOOST_AUTO_TEST_CASE(SignatureVerification_RunsNoProgramOnAGuardedPull)
{
	isolate_config();
	const fs::path remote = make_bare("signed_remote.git");
	const fs::path repo = make_plain_repository("signed");
	git(repo, { "remote", "add", "origin", file_url(remote) });
	git(repo, { "push", "-q", "-u", "origin", "main" });
	const fs::path other = base / "signed_other";
	git(base, { "clone", "-q", file_url(remote), edt::utf8_of(other) });
	git(other, { "config", "user.name", "Other User" });
	git(other, { "config", "user.email", "other@example.com" });
	write(other / "theirs.txt", "theirs\n");
	git(other, { "add", "theirs.txt" });
	git(other, { "commit", "-q", "-m", "their commit" });
	git(other, { "push", "-q", "origin", "main" });
	const fs::path marker = base / "gpg_ran.txt";
	const fs::path script = base / "gpg-ssh.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\nexit 1\n");
	git(repo, { "config", "merge.verifySignatures", "true" });
	git(repo, { "config", "gpg.format", "ssh" });
	git(repo, { "config", "gpg.ssh.program", generic_text(script) });
	const std::string before = head_of(repo);
	make_service(real_options(repo));
	std::error_code ec;

	BOOST_TEST(is_error(finish(service->start_pull({}, {}, cmd::origin_kind::agent)), "unsafe_repository"));
	BOOST_TEST(!fs::exists(marker, ec));

	service->set_write_permissions({ false, false, false, true });
	const json::value overridden = finish(service->start_pull({}, {}, cmd::origin_kind::agent));
	BOOST_TEST(!is_error(overridden, "unsafe_repository"), json::serialize(overridden));
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(head_of(repo) == before);
}

// The allowlist of the repository's own config: an unknown key refuses a guarded write and is named; the config
// `git clone` writes passes; allow_any_repo_config lets the unknown key through.
BOOST_AUTO_TEST_CASE(TheRepositoryConfigAllowlist)
{
	isolate_config();
	const fs::path repo = make_plain_repository("allowlist");
	make_service(real_options(repo));
	const auto agent = cmd::origin_kind::agent;

	for (const char* key : { "core.pager", "foo.bar", "lfs.customtransfer.evil.path", "lfs.extension.evil.clean" }) {
		BOOST_TEST_CONTEXT(key) {
			git(repo, { "config", key, "cat" });
			const json::value refused = finish(service->start_branch_create("topic", {}, agent));
			BOOST_TEST_INFO("refused: " << json::serialize(refused));
			BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
			BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find(key) != std::string::npos);
			git(repo, { "config", "--unset", key });
		}
	}
	git(repo, { "config", "core.pager", "cat" });
	service->set_write_permissions({ false, false, false, true });
	const json::value allowed = finish(service->start_branch_create("topic", {}, agent));
	BOOST_TEST(!allowed.as_object().contains("error"), json::serialize(allowed));

	const fs::path remote = make_bare("allowlist_remote.git");
	git(repo, { "push", "-q", file_url(remote), "main" });
	const fs::path clone = base / "allowlist_clone";
	git(base, { "clone", "-q", file_url(remote), edt::utf8_of(clone) });
	git(clone, { "config", "user.name", "Clone User" });
	git(clone, { "config", "user.email", "clone@example.com" });
	make_service(real_options(clone));
	const json::value in_clone = finish(service->start_branch_create("topic", {}, agent));
	BOOST_TEST(!in_clone.as_object().contains("error"), json::serialize(in_clone));

	// What newer git writes on clone or fetch: data only.
	for (const auto& [key, value] : std::initializer_list<std::pair<const char*, const char*>>{
		{ "remote.origin.followRemoteHEAD", "warn" }, { "remote.origin.promisor", "false" },
		{ "remote.origin.partialclonefilter", "blob:none" }, { "core.sparseCheckout", "false" },
		{ "core.sparseCheckoutCone", "true" }, { "maintenance.auto", "false" }, { "submodule.active", "." },
		{ "submodule.lib.url", "https://example.com/lib.git" }, { "submodule.lib.active", "true" } }) {
		git(clone, { "config", key, value });
	}
	const json::value newer_keys = finish(service->start_branch_create("topic2", {}, agent));
	BOOST_TEST(!newer_keys.as_object().contains("error"), json::serialize(newer_keys));

	git(clone, { "config", "submodule.lib.update", "!touch pwned" });
	const json::value update_command = finish(service->start_branch_create("topic3", {}, agent));
	BOOST_TEST_INFO("refused: " << json::serialize(update_command));
	BOOST_REQUIRE(is_error(update_command, "unsafe_repository"));
	BOOST_TEST(text(update_command.as_object().at("error").as_object().at("message")).find("submodule.lib.update")
		!= std::string::npos);
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
			"GIT_EXTERNAL_DIFF=/evil", "GIT_ALLOW_PROTOCOL=file:ext", "GIT_PROTOCOL_FROM_USER=1", "GIT_CEILING_DIRECTORIES=/kept",
			"HOME=/kept" }) {
		options.environment->emplace_back(entry);
	}
	make_service(std::move(options));
	finish(service->start_status());

	const auto& env = *last_options.environment;
	for (const char* gone : { "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
			"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0",
			"GIT_CONFIG_VALUE_0", "GIT_EXEC_PATH", "GIT_EXTERNAL_DIFF", "GIT_ALLOW_PROTOCOL", "GIT_PROTOCOL_FROM_USER" }) {
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

// An agent's write: hooks point at an empty folder of the editor, askpass is off, ssh is the editor's; filters are
// not switched off (a refusal handles them). A person's write keeps git's own behaviour.
BOOST_AUTO_TEST_CASE(AnAgentsWrite_RunsNoHooksNoAskpassAndTheEditorsSsh)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	auto options = fake_options();
	options.environment->push_back("GIT_ASKPASS=/usr/bin/evil-askpass");
	options.environment->push_back("SSH_ASKPASS=/usr/bin/evil-askpass");
	make_service(std::move(options));

	finish(service->start_stage({ "a.txt", "-rf" }, cmd::origin_kind::agent));
	const auto agent = calls.back();
	BOOST_REQUIRE(agent.size() >= 4u);
	BOOST_TEST(agent[agent.size() - 4] == "add");
	BOOST_TEST(agent[agent.size() - 3] == "--");
	BOOST_TEST(agent.back() == "-rf");
	BOOST_TEST(has_arg(agent, "core.askPass="));
	BOOST_TEST(has_arg(agent, "core.sshCommand=ssh -oBatchMode=yes"));
	BOOST_TEST(has_arg(agent, "core.fsmonitor=false"));
	BOOST_TEST(has_arg(agent, "diff.external="));
	BOOST_TEST(!has_arg(agent, "filter.lfs.clean="));
	const std::string hooks = arg_starting_with(agent, "core.hooksPath=");
	BOOST_REQUIRE(!hooks.empty());
	const fs::path hooks_dir = path_of(std::string_view(hooks).substr(std::string_view("core.hooksPath=").size()));
	std::error_code ec;
	BOOST_TEST(fs::is_directory(hooks_dir, ec));
	BOOST_TEST(fs::is_empty(hooks_dir, ec));
	BOOST_TEST(!fs::equivalent(hooks_dir.parent_path(), base, ec));
	BOOST_TEST(!edt::variable_of(*last_options.environment, "GIT_ASKPASS").has_value());
	BOOST_TEST(!edt::variable_of(*last_options.environment, "SSH_ASKPASS").has_value());

	// Something wrote into the folder: the next agent write gets an empty one again.
	write(hooks_dir / "pre-commit", "#!/bin/sh\n");
	finish(service->start_commit("message", cmd::origin_kind::agent));
	std::size_t commit_call = calls.size();
	while (commit_call > 0 && !has_arg(calls[commit_call - 1], "commit")) --commit_call;
	BOOST_REQUIRE(commit_call > 0);
	const std::string again = arg_starting_with(calls[commit_call - 1], "core.hooksPath=");
	BOOST_REQUIRE(!again.empty());
	BOOST_TEST(fs::is_empty(path_of(std::string_view(again).substr(std::string_view("core.hooksPath=").size())), ec));

	finish(service->start_push("origin", "main", false, cmd::origin_kind::ui));
	const auto person = calls.back();
	BOOST_TEST(arg_starting_with(person, "core.hooksPath=").empty());
	BOOST_TEST(!has_arg(person, "core.askPass="));
	BOOST_TEST(has_arg(person, "core.fsmonitor=false"));
	BOOST_TEST(edt::variable_of(*last_options.environment, "GIT_ASKPASS").value_or("") == "/usr/bin/evil-askpass");
	BOOST_TEST(!has_arg(person, "--force"));
	BOOST_REQUIRE(person.size() >= 3u);
	BOOST_TEST(person[person.size() - 3] == "--end-of-options");
	BOOST_TEST(person[person.size() - 2] == "origin");
	BOOST_TEST(person.back() == "main");

	BOOST_TEST(arg_starting_with(person, "remote.origin.uploadpack=").empty());

	service->set_write_permissions({ true, false, false, false });
	finish(service->start_fetch({}, cmd::origin_kind::agent));
	const auto allowed = calls.back();
	BOOST_TEST(has_arg(allowed, "fetch"));
	BOOST_TEST(arg_starting_with(allowed, "core.hooksPath=").empty());
	for (const char* config : { "core.askPass=", "protocol.allow=never", "protocol.https.allow=always",
			"protocol.ssh.allow=always", "protocol.ext.allow=never", "commit.gpgsign=false", "tag.gpgsign=false",
			"merge.verifySignatures=false", "gpg.program=", "gpg.openpgp.program=", "gpg.x509.program=", "gpg.ssh.program=",
			"gpg.ssh.defaultKeyCommand=", "core.gitProxy=", "core.alternateRefsCommand=",
			"remote.origin.uploadpack=git-upload-pack", "remote.origin.receivepack=git-receive-pack" }) {
		BOOST_TEST_CONTEXT(config) {
			BOOST_TEST(has_arg(allowed, config));
		}
	}
	BOOST_TEST(edt::variable_of(*last_options.environment, "GIT_PROXY_COMMAND").value_or("x").empty());
	// The repository's own config was read first.
	BOOST_TEST(std::any_of(calls.begin(), calls.end(), [](const std::vector<std::string>& call) {
		return has_arg(call, "--local") && has_arg(call, "--list") && has_arg(call, "--show-origin");
	}));

	// test and game are guarded like agent; only ui is a person.
	for (const auto caller : { cmd::origin_kind::test, cmd::origin_kind::game }) {
		finish(service->start_branch_create("topic", {}, caller));
		BOOST_TEST(has_arg(calls.back(), "protocol.allow=never"));
	}
	finish(service->start_branch_create("topic", {}, cmd::origin_kind::ui));
	BOOST_TEST(!has_arg(calls.back(), "protocol.allow=never"));

	// Glob characters in a guarded branch or ref name.
	for (const char* name : { "a*", "a?", "a[b" }) {
		BOOST_TEST(service->start_branch_create(name, {}, cmd::origin_kind::agent).code == cmd::codes::invalid_args, name);
		BOOST_TEST(service->start_checkout(name, cmd::origin_kind::agent).code == cmd::codes::invalid_args, name);
		BOOST_TEST(service->start_push("origin", name, false, cmd::origin_kind::agent).code == cmd::codes::invalid_args, name);
	}
}

// The guarded pull: the merge gets the OID the FETCH_HEAD line named (peeled once), with the signature overrides.
BOOST_AUTO_TEST_CASE(AGuardedPull_MergesTheOidItChecked)
{
	write(base / "FETCH_HEAD", "1111111111111111111111111111111111111111\t\tbranch 'main' of /elsewhere\n");
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	auto options = fake_options();
	options.run = [inner = options.run](const fs::path& git, const std::vector<std::string>& args,
		const platform::process_options& opts, const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		if (has_arg(args, "1111111111111111111111111111111111111111^{commit}")) {
			on_line("2222222222222222222222222222222222222222\n");
			return exited(0);
		}
		return inner(git, args, opts, on_line, stop);
	};
	make_service(std::move(options));
	const json::value result = finish(service->start_pull({}, {}, cmd::origin_kind::agent));
	BOOST_TEST(!result.as_object().contains("error"), json::serialize(result));
	const auto& merge = calls.back();
	BOOST_TEST(has_arg(merge, "merge"));
	BOOST_TEST(merge.back() == "2222222222222222222222222222222222222222");
	BOOST_TEST(has_arg(merge, "merge.verifySignatures=false"));
	BOOST_TEST(has_arg(merge, "gpg.ssh.program="));
	BOOST_TEST(!has_arg(merge, "FETCH_HEAD"));
}

// A configured driver refuses an agent's write only when check-attr names it for a file the write touches; a
// .gitattributes a checkout or pull would change refuses it whatever check-attr says.
BOOST_AUTO_TEST_CASE(AFilterDriver_RefusesAnAgentsTreeWrite_OnlyWhereItApplies)
{
	filter_config = "filter.lfs.clean\ngit-lfs clean -- %f\0filter.lfs.smudge\ngit-lfs smudge -- %f\0"
		"filter.lfs.required\ntrue\0filter.crypt.process\ncrypt-filter\0"s;
	write(base / "FETCH_HEAD", "0123456789abcdef\t\tbranch 'main' of /elsewhere\n");
	std::string changed = "a.bin\0"s;
	std::vector<std::string> ran;
	command = [&ran, &changed](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		const auto after_dash_dash = [&args] {
			const auto dash_dash = std::find(args.begin(), args.end(), "--");
			return dash_dash == args.end() ? std::vector<std::string>{} : std::vector<std::string>(std::next(dash_dash), args.end());
		};
		if (has_arg(args, "ls-files")) {
			// The whole index is `changed`; --others: the paths asked for.
			if (has_arg(args, "--others")) {
				for (const auto& path : after_dash_dash()) on_line(path + '\0');
			}
			else {
				on_line(changed);
			}
			return exited(0);
		}
		if (has_arg(args, "--name-only")) {
			on_line(changed);
			return exited(0);
		}
		if (has_arg(args, "check-attr")) {
			for (const auto& path : after_dash_dash()) {
				on_line(path + '\0' + "filter" + '\0' + (path.ends_with(".bin") ? "lfs" : "unspecified") + '\0');
			}
			return exited(0);
		}
		for (const char* subcommand : { "add", "reset", "commit", "checkout", "merge", "branch", "fetch", "push" }) {
			if (has_arg(args, subcommand)) ran.emplace_back(subcommand);
		}
		return exited(0);
	};
	make_service(fake_options());
	const auto agent = cmd::origin_kind::agent;
	const auto refused_with_lfs = [](const json::value& result) {
		if (!is_error(result, "filters_require_opt_in")) return false;
		const std::string message = text(result.as_object().at("error").as_object().at("message"));
		return message.find("lfs") != std::string::npos && message.find("crypt") == std::string::npos;
	};

	BOOST_TEST(refused_with_lfs(finish(service->start_stage({ "a.bin" }, agent))));
	BOOST_TEST(refused_with_lfs(finish(service->start_unstage({ "a.bin" }, agent))));
	BOOST_TEST(refused_with_lfs(finish(service->start_commit("message", agent))));
	BOOST_TEST(refused_with_lfs(finish(service->start_checkout("main", agent))));
	BOOST_TEST(ran.empty());
	BOOST_TEST(refused_with_lfs(finish(service->start_pull({}, {}, agent))));
	BOOST_TEST((ran == std::vector<std::string>{ "fetch" }));

	// Configured, but no attribute names a driver for these files: git runs.
	ran.clear();
	changed = "a.txt\0"s;
	BOOST_TEST(!finish(service->start_stage({ "a.txt" }, agent)).as_object().contains("error"));
	BOOST_TEST(!finish(service->start_commit("message", agent)).as_object().contains("error"));
	BOOST_TEST(!finish(service->start_checkout("main", agent)).as_object().contains("error"));
	BOOST_TEST(!finish(service->start_pull({}, {}, agent)).as_object().contains("error"));
	BOOST_TEST((ran == std::vector<std::string>{ "add", "commit", "checkout", "fetch", "merge" }));

	// A .gitattributes the target changes: the work tree's attributes say nothing about the target's.
	ran.clear();
	changed = "a.txt\0sub/.gitattributes\0"s;
	BOOST_TEST(is_error(finish(service->start_checkout("main", agent)), "filters_require_opt_in"));
	BOOST_TEST(ran.empty());

	// Writes that touch no file are not checked.
	finish(service->start_branch_create("topic", {}, agent));
	finish(service->start_fetch("origin", agent));
	finish(service->start_push("origin", "topic", true, agent));
	BOOST_TEST((ran == std::vector<std::string>{ "branch", "fetch", "push" }));

	// A person's write runs git, filters and all.
	ran.clear();
	changed = "a.bin\0"s;
	BOOST_TEST(!finish(service->start_commit("message", cmd::origin_kind::ui)).as_object().contains("error"));
	BOOST_TEST((ran == std::vector<std::string>{ "commit" }));

	service->set_write_permissions({ false, true, false, false });
	ran.clear();
	BOOST_TEST(!finish(service->start_commit("message", agent)).as_object().contains("error"));
	BOOST_TEST((ran == std::vector<std::string>{ "commit" }));
}

// What an agent's network call refuses in the repository's config (git config --show-scope), and what it lets by.
BOOST_AUTO_TEST_CASE(AnAgentsNetworkCall_RefusesProgramsTheRepositoryConfigPicks)
{
	bool pushed = false;
	command = [&pushed](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) {
		if (has_arg(args, "push")) pushed = true;
		return exited(0);
	};
	make_service(fake_options());
	const auto push = [this, &pushed](cmd::origin_kind caller) {
		pushed = false;
		return finish(service->start_push("origin", "main", false, caller));
	};

	for (const auto& unsafe : { "local\0credential.helper\n!touch marker\0"s, "worktree\0credential.https://x.helper\nstore\0"s,
			"local\0core.sshcommand\nevil-ssh\0"s, "local\0url.file:///srv/evil.git.insteadof\nhttps://example.com/\0"s,
			"local\0url.ext::sh -c evil.pushinsteadof\ngit@example.com:\0"s, "global\0remote.origin.uploadpack\nevil-pack\0"s,
			"local\0remote.origin.receivepack\nevil-pack\0"s, "local\0remote.other.receivepack\nevil-pack\0"s }) {
		filter_config = unsafe;
		BOOST_TEST_CONTEXT(unsafe) {
			const json::value result = push(cmd::origin_kind::agent);
			BOOST_TEST(is_error(result, "unsafe_repository"), json::serialize(result));
			BOOST_TEST(!pushed);
			push(cmd::origin_kind::ui);
			BOOST_TEST(pushed);
		}
	}

	for (const auto& fine : { "global\0credential.helper\nmanager\0"s, "system\0credential.helper\nstore\0"s,
			"local\0credential.helper\n\0"s, "global\0core.sshcommand\nssh -i key\0"s,
			"local\0url.https://mirror.example.com/.insteadof\nhttps://example.com/\0"s,
			"local\0url.git@example.com:.pushinsteadof\nhttps://example.com/\0"s,
			"local\0remote.origin.uploadpack\ngit-upload-pack\0"s }) {
		filter_config = fine;
		BOOST_TEST_CONTEXT(fine) {
			const json::value result = push(cmd::origin_kind::agent);
			BOOST_TEST(!result.as_object().contains("error"), json::serialize(result));
			BOOST_TEST(pushed);
		}
	}

	filter_config = "local\0credential.helper\n!touch marker\0"s;
	service->set_write_permissions({ false, false, true, false });
	BOOST_TEST(!push(cmd::origin_kind::agent).as_object().contains("error"));
	BOOST_TEST(pushed);
}

// A job.cancel while a write runs stops git; the commit message file goes too.
BOOST_AUTO_TEST_CASE(Cancel_StopsARunningCommit_AndRemovesItsMessageFile)
{
	edt::job_service::job_id running = 0;
	bool saw_stop = false;
	fs::path message_file;
	std::string written;
	command = [this, &running, &saw_stop, &message_file, &written](const std::vector<std::string>& args,
		const platform::process_options&, const std::function<void(std::string_view)>&, std::stop_token stop) {
		const auto flag = std::find(args.begin(), args.end(), "-F");
		if (flag != args.end() && std::next(flag) != args.end()) {
			message_file = path_of(*std::next(flag));
			std::ifstream in(message_file, std::ios::binary);
			written.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		jobs->cancel(running);
		const platform::process_result result = hang_until_stopped(stop);
		saw_stop = result.cancelled;
		return result;
	};
	make_service(fake_options());
	const std::string message = "line one\nline two \xE2\x9C\x93\n";
	const auto started = service->start_commit(message, cmd::origin_kind::ui);
	BOOST_REQUIRE(static_cast<bool>(started));
	running = started.job;
	run_to_end();

	BOOST_TEST(saw_stop);
	BOOST_TEST(written == message);
	BOOST_REQUIRE(!message_file.empty());
	std::error_code ec;
	BOOST_TEST(!fs::exists(message_file, ec));
	BOOST_TEST(!fs::equivalent(message_file.parent_path(), base, ec));
	const auto info = jobs->info(running);
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST((info->status == edt::job_status::cancelled));
	BOOST_TEST(!info->result.has_value());
}

// The network checks need git config --show-scope (git 2.26): an older git refuses an agent's network call at once.
BOOST_AUTO_TEST_CASE(AGitOlderThan2_26_RefusesAnAgentsNetworkCall)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	auto options = fake_options();
	options.run = [inner = options.run](const fs::path& git, const std::vector<std::string>& args,
		const platform::process_options& opts, const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		if (has_arg(args, "--version")) {
			on_line("git version 2.25.4\n");
			return exited(0);
		}
		return inner(git, args, opts, on_line, stop);
	};
	make_service(std::move(options));
	BOOST_REQUIRE(service->info().available);
	const std::string expected = "git 2.26 or newer is required for agent network operations (found 2.25.4)";

	const auto agent = cmd::origin_kind::agent;
	for (const auto& started : { service->start_fetch({}, agent), service->start_pull({}, {}, agent),
			service->start_push("origin", "main", false, agent) }) {
		BOOST_TEST(started.code == "vcs_unavailable");
		BOOST_TEST(started.error == expected);
	}
	BOOST_TEST(jobs->list().empty());
	// A person's network call and an agent's local write still run.
	BOOST_TEST(!finish(service->start_push("origin", "main", false, cmd::origin_kind::ui)).as_object().contains("error"));
	BOOST_TEST(!finish(service->start_branch_create("topic", {}, agent)).as_object().contains("error"));
}

BOOST_AUTO_TEST_SUITE_END()
