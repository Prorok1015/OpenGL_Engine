#include <boost/test/unit_test.hpp>
#include "edt_vcs_service.h"
#include "edt_path_text.h"
#include "edt_agent_policy.h"
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_service_commands.h>
#include <snk/platform/plt_process.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
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

// As `git config --list` prints a key whose subsection is lower case already.
std::string lower_key(std::string_view key)
{
	std::string out(key);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	return out;
}

// "2.45.1", "2.39.2.windows.1": at least major.minor.
bool git_at_least(std::string_view version, int major, int minor)
{
	const char* const end = version.data() + version.size();
	int found_major = 0;
	const auto [after_major, major_error] = std::from_chars(version.data(), end, found_major);
	if (major_error != std::errc{} || after_major == end || *after_major != '.') return false;
	int found_minor = 0;
	if (std::from_chars(after_major + 1, end, found_minor).ec != std::errc{}) return false;
	return found_major > major || (found_major == major && found_minor >= minor);
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

	// An empty commit by `author` with author and committer date `time` (unix seconds).
	void commit_at(const fs::path& repo, std::string_view message, std::int64_t time, std::string_view author = "Test User")
	{
		const std::vector<std::string> saved = environment;
		edt::set_variable(environment, "GIT_AUTHOR_DATE", std::format("{} +0000", time));
		edt::set_variable(environment, "GIT_COMMITTER_DATE", std::format("{} +0000", time));
		git(repo, { "-c", "user.name=" + std::string(author), "commit", "-q", "--allow-empty", "-m", std::string(message) });
		environment = saved;
	}

	// <base>/<name>: an empty repository on main.
	fs::path make_empty_repository(std::string_view name)
	{
		const fs::path repo = base / name;
		fs::create_directories(repo);
		git(repo, { "init", "-q" });
		git(repo, { "symbolic-ref", "HEAD", "refs/heads/main" });
		git(repo, { "config", "user.name", "Test User" });
		git(repo, { "config", "user.email", "test@example.com" });
		git(repo, { "config", "commit.gpgsign", "false" });
		git(repo, { "config", "tag.gpgsign", "false" });
		git(repo, { "config", "core.autocrlf", "false" });
		return repo;
	}

	std::vector<std::string> subjects_of(const json::value& page)
	{
		std::vector<std::string> out;
		for (const auto& commit : page.as_object().at("commits").as_array()) out.push_back(text(commit.as_object().at("subject")));
		return out;
	}

	std::vector<std::string> hashes_of(const json::value& page)
	{
		std::vector<std::string> out;
		for (const auto& commit : page.as_object().at("commits").as_array()) out.push_back(text(commit.as_object().at("hash")));
		return out;
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
				on_line(edt::utf8_of(base / path_of(args.back())) + "\n");
				return exited(0);
			}
			// rev-parse of a revision (--end-of-options) goes to `command`.
			if (has_arg(args, "rev-parse") && !has_arg(args, "--end-of-options")) {
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
	BOOST_TEST(registrations.size() == 14u);

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
	BOOST_TEST(commands.invoke("vcs.log", json::parse(R"({"all":true,"ref":"main"})"), cmd::origin::test()).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("vcs.show_files", json::object{}, cmd::origin::test()).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("vcs.show_files", json::parse(R"({"commit":"HEAD"})"), cmd::origin::test()).code == cmd::codes::invalid_args);
	const std::string head = head_of(repo);
	BOOST_TEST(commands.invoke("vcs.diff", json::object{ { "commit", head }, { "staged", true } }, cmd::origin::test()).code
		== cmd::codes::invalid_args);
	const cmd::result files = commands.invoke("vcs.show_files", json::object{ { "commit", head } }, cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(files.ok, files.error);
	const json::value files_result = finish(static_cast<edt::job_service::job_id>(files.value.as_object().at("job").as_int64()));
	BOOST_TEST(files_result.as_object().at("files").as_array().size() == 1u, json::serialize(files_result));
	const cmd::result diff =commands.invoke("vcs.diff", json::parse(R"({"path":"a.txt","staged":false})"), cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(diff.ok, diff.error);
	const json::value diff_result = finish(static_cast<edt::job_service::job_id>(diff.value.as_object().at("job").as_int64()));
	BOOST_TEST(text(diff_result.as_object().at("text")).find("+two") != std::string::npos);

	for (const char* id : { "vcs.info", "vcs.status", "vcs.diff", "vcs.log", "vcs.show_files", "vcs.branches" }) {
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

// main: m1 (1000) - m2 (3000); feature from m1: f1 (2000, annotated tag v1) - f2 (4000); origin/main at m2.
BOOST_AUTO_TEST_CASE(RealRepository_LogOfAll_ShowsEveryBranchAndTag_WithTheirRefs_ByDate)
{
	isolate_config();
	const fs::path repo = make_empty_repository("history");
	commit_at(repo, "m1", 1000);
	git(repo, { "branch", "feature" });
	commit_at(repo, "m2", 3000);
	git(repo, { "checkout", "-q", "feature" });
	commit_at(repo, "f1", 2000);
	git(repo, { "tag", "-a", "v1", "-m", "tag v1" });
	commit_at(repo, "f2", 4000);
	git(repo, { "checkout", "-q", "main" });
	git(repo, { "update-ref", "refs/remotes/origin/main", "refs/heads/main" });
	git(repo, { "symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main" });
	make_service(real_options(repo));

	edt::vcs_service::log_filter all;
	all.all = true;
	const json::value page = finish(service->start_log({}, {}, 50, {}, all));
	BOOST_TEST_INFO("log: " << json::serialize(page));
	BOOST_TEST((subjects_of(page) == std::vector<std::string>{ "f2", "m2", "f1", "m1" }));
	BOOST_TEST(!page.as_object().contains("next_cursor"));
	const auto refs_of = [&page](std::string_view subject) {
		std::vector<std::string> out;
		for (const auto& commit : page.as_object().at("commits").as_array()) {
			if (commit.as_object().at("subject").as_string() != subject) continue;
			for (const auto& ref : commit.as_object().at("refs").as_array()) {
				out.push_back(text(ref.as_object().at("kind")) + ":" + text(ref.as_object().at("name")));
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	};
	BOOST_TEST((refs_of("f2") == std::vector<std::string>{ "branch:feature" }));
	BOOST_TEST((refs_of("m2") == std::vector<std::string>{ "branch:main", "head:HEAD", "remote:origin/main" }));
	BOOST_TEST((refs_of("f1") == std::vector<std::string>{ "tag:v1" }));
	BOOST_TEST(refs_of("m1").empty());

	BOOST_TEST((subjects_of(finish(service->start_log({}, {}, 50, {}))) == std::vector<std::string>{ "m2", "m1" }));
	BOOST_TEST((subjects_of(finish(service->start_log("v1", {}, 50, {}))) == std::vector<std::string>{ "f1", "m1" }));
	BOOST_TEST(is_error(finish(service->start_log("no-such-branch", {}, 50, {})), "bad_revision"));
}

BOOST_AUTO_TEST_CASE(RealRepository_LogFilters_AreLiteralAndIgnoreCase)
{
	isolate_config();
	const fs::path repo = make_empty_repository("filters");
	commit_at(repo, "Fix [a-z]+ parser", 1000, "Ann Example");
	commit_at(repo, "fix lexer", 2000, "Bob Builder");
	commit_at(repo, "add docs", 3000, "Ann Example");
	make_service(real_options(repo));

	const auto log_of = [this](std::string author, std::string grep) {
		edt::vcs_service::log_filter filter;
		filter.author = std::move(author);
		filter.grep = std::move(grep);
		return subjects_of(finish(service->start_log({}, {}, 50, {}, filter)));
	};
	BOOST_TEST((log_of("ann", {}) == std::vector<std::string>{ "add docs", "Fix [a-z]+ parser" }));
	BOOST_TEST((log_of({}, "FIX") == std::vector<std::string>{ "fix lexer", "Fix [a-z]+ parser" }));
	// As a pattern "[A-Z]+" would match every message, "." too.
	BOOST_TEST((log_of({}, "[A-Z]+") == std::vector<std::string>{ "Fix [a-z]+ parser" }));
	BOOST_TEST(log_of({}, ".").empty());
	BOOST_TEST((log_of("BOB", "fix") == std::vector<std::string>{ "fix lexer" }));
	BOOST_TEST(log_of("example.com", {}).size() == 3u);

	edt::vcs_service::log_filter bad;
	bad.grep = "a\nb";
	BOOST_TEST(service->start_log({}, {}, 10, {}, bad).code == cmd::codes::invalid_args);
	bad.grep = std::string(edt::vcs_service::k_max_filter_bytes + 1, 'a');
	BOOST_TEST(service->start_log({}, {}, 10, {}, bad).code == cmd::codes::invalid_args);
	bad.grep.clear();
	bad.author = "x\x7F";
	BOOST_TEST(service->start_log({}, {}, 10, {}, bad).code == cmd::codes::invalid_args);
	bad.author.clear();
	bad.all = true;
	BOOST_TEST(service->start_log("main", {}, 10, {}, bad).code == cmd::codes::invalid_args);
}

// Owner decision (#457): the first page keeps its tips; later pages walk them whatever was committed meanwhile.
BOOST_AUTO_TEST_CASE(RealRepository_LogPages_FollowTheirSnapshot_WhateverIsCommittedMeanwhile)
{
	isolate_config();
	const fs::path repo = make_empty_repository("pages");
	commit_at(repo, "c1", 1000);
	commit_at(repo, "c2", 2000);
	commit_at(repo, "c3", 3000);
	make_service(real_options(repo));

	const json::value first = finish(service->start_log({}, {}, 2, {}));
	BOOST_TEST((subjects_of(first) == std::vector<std::string>{ "c3", "c2" }));
	BOOST_REQUIRE(first.as_object().contains("next_cursor"));
	const std::string cursor = text(first.as_object().at("next_cursor"));

	commit_at(repo, "c4", 4000);
	const json::value second = finish(service->start_log({}, {}, 2, cursor));
	BOOST_TEST((subjects_of(second) == std::vector<std::string>{ "c1" }));
	BOOST_TEST(!second.as_object().contains("next_cursor"));
	BOOST_TEST((hashes_of(finish(service->start_log({}, {}, 2, cursor))) == hashes_of(second)));
	BOOST_TEST((subjects_of(finish(service->start_log({}, {}, 2, {}))) == std::vector<std::string>{ "c4", "c3" }));

	// The cursor of another query, or of a snapshot that is not kept: stale, as the job's result.
	edt::vcs_service::log_filter other;
	other.grep = "c";
	BOOST_TEST(is_error(finish(service->start_log({}, {}, 2, cursor, other)), "stale"));
	BOOST_TEST(is_error(finish(service->start_log({}, "a.txt", 2, cursor)), "stale"));
	BOOST_TEST(is_error(finish(service->start_log("main", {}, 2, cursor)), "stale"));
	BOOST_TEST(is_error(finish(service->start_log({}, {}, 2, "s999:2")), "stale"));
	// Not shaped as vcs.log's cursors: invalid_args before any job.
	for (const char* bad : { "2", "abc", "s0:2", "s1", "s1:", "s1:-1", "s1:+1", "sx:2", "s1:2x", "S1:2" }) {
		BOOST_TEST(service->start_log({}, {}, 2, bad).code == cmd::codes::invalid_args, bad);
	}
}

BOOST_AUTO_TEST_CASE(RealRepository_LogSnapshots_KeepTheEightMostRecentlyUsed)
{
	isolate_config();
	const fs::path repo = make_empty_repository("snapshots");
	commit_at(repo, "c1", 1000);
	commit_at(repo, "c2", 2000);
	make_service(real_options(repo));
	const auto first_page = [this] {
		const json::value page = finish(service->start_log({}, {}, 1, {}));
		BOOST_REQUIRE(page.as_object().contains("next_cursor"));
		return text(page.as_object().at("next_cursor"));
	};
	const auto second_page = [this](const std::string& cursor) { return finish(service->start_log({}, {}, 1, cursor)); };

	std::vector<std::string> cursors;
	for (std::size_t i = 0; i <= edt::vcs_service::k_max_log_snapshots; ++i) cursors.push_back(first_page());
	BOOST_TEST(is_error(second_page(cursors[0]), "stale"));
	for (std::size_t i = 1; i < cursors.size(); ++i) {
		BOOST_TEST((subjects_of(second_page(cursors[i])) == std::vector<std::string>{ "c1" }), i);
	}
	// Used again, the oldest one stays; the next oldest goes.
	BOOST_TEST(!is_error(second_page(cursors[1]), "stale"));
	cursors.push_back(first_page());
	BOOST_TEST(is_error(second_page(cursors[2]), "stale"));
	BOOST_TEST(!is_error(second_page(cursors[1]), "stale"));
}

BOOST_AUTO_TEST_CASE(RealRepository_ShowFilesAndCommitDiff_OfARootANormalARenameAndAMergeCommit)
{
	isolate_config();
	const fs::path repo = make_plain_repository("files");
	const std::string root = head_of(repo);
	write(repo / "a.txt", "one\ntwo\n");
	write(repo / "b.txt", "bee\nbee\nbee\n");
	git(repo, { "add", "a.txt", "b.txt" });
	git(repo, { "commit", "-q", "-m", "change a, add b" });
	const std::string normal = head_of(repo);
	git(repo, { "mv", "b.txt", "c.txt" });
	git(repo, { "commit", "-q", "-m", "rename b" });
	const std::string renamed = head_of(repo);
	git(repo, { "checkout", "-q", "-b", "side" });
	write(repo / "side.txt", "side\n");
	git(repo, { "add", "side.txt" });
	git(repo, { "commit", "-q", "-m", "side" });
	git(repo, { "checkout", "-q", "main" });
	write(repo / "main.txt", "main\n");
	git(repo, { "add", "main.txt" });
	git(repo, { "commit", "-q", "-m", "main" });
	git(repo, { "merge", "-q", "--no-ff", "--no-edit", "side" });
	const std::string merge = head_of(repo);
	make_service(real_options(repo));

	const auto files_of = [this](const std::string& commit) {
		const json::value result = finish(service->start_show_files(commit));
		BOOST_TEST_INFO("show_files: " << json::serialize(result));
		BOOST_REQUIRE(result.as_object().contains("files"));
		std::vector<std::string> out;
		for (const auto& item : result.as_object().at("files").as_array()) {
			const json::object& file = item.as_object();
			std::string line = text(file.at("status")) + " " + text(file.at("path"));
			if (file.contains("orig_path")) line += " <- " + text(file.at("orig_path"));
			out.push_back(std::move(line));
		}
		return out;
	};
	BOOST_TEST((files_of(root) == std::vector<std::string>{ "A a.txt" }));
	BOOST_TEST((files_of(normal) == std::vector<std::string>{ "M a.txt", "A b.txt" }));
	BOOST_TEST((files_of(renamed) == std::vector<std::string>{ "R c.txt <- b.txt" }));
	BOOST_TEST((files_of(renamed.substr(0, 8)) == std::vector<std::string>{ "R c.txt <- b.txt" }));
	// A merge against its first parent: what the merged branch brought.
	BOOST_TEST((files_of(merge) == std::vector<std::string>{ "A side.txt" }));

	const json::value change = finish(service->start_commit_diff(normal, "a.txt"));
	BOOST_TEST_INFO("diff: " << json::serialize(change));
	BOOST_TEST(text(change.as_object().at("text")).find("+two") != std::string::npos);
	BOOST_TEST(text(change.as_object().at("text")).find("b.txt") == std::string::npos);
	BOOST_TEST(!change.as_object().at("truncated").as_bool());
	BOOST_TEST(text(finish(service->start_commit_diff(root, "a.txt")).as_object().at("text")).find("+one") != std::string::npos);
	const std::string merged = text(finish(service->start_commit_diff(merge, {})).as_object().at("text"));
	BOOST_TEST(merged.find("side.txt") != std::string::npos);
	BOOST_TEST(merged.find("main.txt") == std::string::npos);

	// Only 4..64 hex digits reach git; an unknown one, or one that is no commit, is bad_revision.
	const std::size_t before = jobs->list().size();
	for (const std::string& bad : { "abc"s, std::string(65, 'a'), "abcg"s, "-abcd"s, "HEAD"s, "abcd^1"s, ""s }) {
		BOOST_TEST(service->start_show_files(bad).code == cmd::codes::invalid_args, bad);
		BOOST_TEST(service->start_commit_diff(bad, "a.txt").code == cmd::codes::invalid_args, bad);
	}
	BOOST_TEST(jobs->list().size() == before);
	BOOST_TEST(is_error(finish(service->start_show_files("0123456789abcdef0123")), "bad_revision"));
	BOOST_TEST(is_error(finish(service->start_commit_diff("0123456789abcdef0123", "a.txt")), "bad_revision"));
	std::string blob = git_text(repo, { "rev-parse", "HEAD:a.txt" });
	while (!blob.empty() && (blob.back() == '\n' || blob.back() == '\r')) blob.pop_back();
	BOOST_TEST(is_error(finish(service->start_show_files(blob)), "bad_revision"));
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
	edt::vcs_service::log_filter every;
	every.all = true;
	every.author = "test";
	every.grep = "CO";
	const json::value all = finish(service->start_log({}, {}, 1, {}, every));
	BOOST_TEST(!all.as_object().contains("error"), json::serialize(all));
	BOOST_REQUIRE(all.as_object().contains("next_cursor"));
	finish(service->start_log({}, {}, 1, text(all.as_object().at("next_cursor")), every));
	const std::string head = head_of(repo);
	const json::value files = finish(service->start_show_files(head));
	BOOST_TEST(!files.as_object().contains("error"), json::serialize(files));
	const json::value committed = finish(service->start_commit_diff(head, {}));
	BOOST_TEST(text(committed.as_object().at("text")).find("+bee") != std::string::npos, json::serialize(committed));
	finish(service->start_commit_diff(head, "dir with space/b.txt"));

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
	git(repo, { "branch", "agent/x" });
	make_service(real_options(repo));
	std::error_code ec;

	const json::value refused = finish(service->start_push("origin", "agent/x", false, cmd::origin_kind::agent));
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
	const json::value allowed = finish(service->start_push("origin", "agent/x", false, cmd::origin_kind::agent));
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

	const json::value refused = finish(service->start_push("origin", "agent/x", false, cmd::origin_kind::agent));
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

// The writes are mutating commands of the vcs_* categories (#455); the reads are vcs_read.
BOOST_AUTO_TEST_CASE(WriteCommands_MutateAndHaveTheirVcsCategories)
{
	isolate_config();
	const fs::path repo = make_plain_repository("commands");
	make_service(real_options(repo));
	registrations = edt::register_vcs_commands(commands, *service);
	using cmd::agent_category;
	const std::pair<const char*, agent_category> writes[] = { { "vcs.stage", agent_category::vcs_commit },
		{ "vcs.unstage", agent_category::vcs_commit }, { "vcs.commit", agent_category::vcs_commit },
		{ "vcs.branch.create", agent_category::vcs_branch }, { "vcs.checkout", agent_category::vcs_branch },
		{ "vcs.fetch", agent_category::vcs_sync }, { "vcs.pull", agent_category::vcs_sync }, { "vcs.push", agent_category::vcs_push } };
	for (const auto& [id, category] : writes) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* desc = commands.find(id);
			BOOST_REQUIRE(desc != nullptr);
			BOOST_TEST(desc->mutates);
			BOOST_TEST((desc->category == category));
		}
	}
	for (const char* id : { "vcs.info", "vcs.status", "vcs.diff", "vcs.log", "vcs.show_files", "vcs.branches" }) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* desc = commands.find(id);
			BOOST_REQUIRE(desc != nullptr);
			BOOST_TEST(!desc->mutates);
			BOOST_TEST((desc->category == agent_category::vcs_read));
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
	git(repo, { "branch", "agent/x" });
	make_service(real_options(repo));
	std::error_code ec;

	for (const bool any_config : { false, true }) {
		BOOST_TEST_CONTEXT("allow_any_repo_config " << any_config) {
			service->set_write_permissions({ false, false, false, any_config });
			const json::value refused = finish(service->start_push("origin", "agent/x", false, cmd::origin_kind::agent));
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
		{ "remote.origin.followRemoteHEAD", "warn" }, { "core.sparseCheckout", "false" },
		{ "core.sparseCheckoutCone", "true" }, { "maintenance.auto", "false" }, { "submodule.active", "." },
		{ "submodule.lib.url", "https://example.com/lib.git" }, { "submodule.lib.active", "true" } }) {
		git(clone, { "config", key, value });
	}
	const json::value newer_keys = finish(service->start_branch_create("topic2", {}, agent));
	BOOST_TEST(!newer_keys.as_object().contains("error"), json::serialize(newer_keys));

	// A partial clone fetches missing objects on demand, past the network checks: refused, as any other extension.
	for (const auto& [key, value] : std::initializer_list<std::pair<const char*, const char*>>{
		{ "remote.origin.promisor", "true" }, { "remote.origin.partialclonefilter", "blob:none" },
		{ "extensions.partialClone", "origin" }, { "extensions.noop", "true" } }) {
		BOOST_TEST_CONTEXT(key) {
			git(clone, { "config", key, value });
			const json::value refused = finish(service->start_branch_create("topic4", {}, agent));
			BOOST_TEST_INFO("refused: " << json::serialize(refused));
			BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
			BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find(lower_key(key)) != std::string::npos);
			git(clone, { "config", "--unset", key });
		}
	}

	git(clone, { "config", "submodule.lib.update", "!touch pwned" });
	const json::value update_command = finish(service->start_branch_create("topic3", {}, agent));
	BOOST_TEST_INFO("refused: " << json::serialize(update_command));
	BOOST_REQUIRE(is_error(update_command, "unsafe_repository"));
	BOOST_TEST(text(update_command.as_object().at("error").as_object().at("message")).find("submodule.lib.update")
		!= std::string::npos);
}

// Second review of #474 (N1): a bare `worktreeConfig` key is true to git, and config.worktree then sets what it
// likes (protocol.file.allow beats the -c fallback). The worktree's config file is read whatever the extension says.
BOOST_AUTO_TEST_CASE(TheWorktreeConfigFile_IsCheckedWhateverTheExtensionSays)
{
	isolate_config();
	const fs::path repo = make_plain_repository("worktree_config");
	{
		std::ofstream config(repo / ".git" / "config", std::ios::binary | std::ios::app);
		const std::string bare_key = "[extensions]\n\tworktreeConfig\n";
		config.write(bare_key.data(), static_cast<std::streamsize>(bare_key.size()));
	}
	write(repo / ".git" / "config.worktree", "[protocol \"file\"]\n\tallow = always\n");
	make_service(real_options(repo));

	const json::value refused = finish(service->start_branch_create("topic", {}, cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("protocol.file.allow") != std::string::npos);

	// Without the extension git ignores the file, and it is still checked.
	const fs::path plain = make_plain_repository("worktree_config_plain");
	write(plain / ".git" / "config.worktree", "[core]\n\tpager = cat\n");
	make_service(real_options(plain));
	const json::value plain_refused = finish(service->start_branch_create("topic", {}, cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(plain_refused));
	BOOST_REQUIRE(is_error(plain_refused, "unsafe_repository"));
	BOOST_TEST(text(plain_refused.as_object().at("error").as_object().at("message")).find("core.pager") != std::string::npos);
}

// Second review of #474 (N2): a partial clone fetches a missing object from its promisor remote on demand, past the
// network checks (and with the repository's credential helper). extensions.partialClone is refused; with
// allow_any_repo_config, GIT_NO_LAZY_FETCH (git 2.45+) keeps the checkout from fetching: GIT_TRACE shows no fetch.
BOOST_AUTO_TEST_CASE(APartialClone_LazyFetchesNothingOnAGuardedCheckout)
{
	isolate_config();
	const fs::path repo = make_plain_repository("partial");
	git(repo, { "checkout", "-q", "-b", "other" });
	write(repo / "big.txt", "a blob the promisor would have to send\n");
	git(repo, { "add", "big.txt" });
	git(repo, { "commit", "-q", "-m", "big" });
	git(repo, { "checkout", "-q", "main" });
	std::string blob = git_text(repo, { "rev-parse", "other:big.txt" });
	while (!blob.empty() && (blob.back() == '\n' || blob.back() == '\r')) blob.pop_back();
	BOOST_REQUIRE(blob.size() > 2);
	std::error_code ec;
	const fs::path object = repo / ".git" / "objects" / blob.substr(0, 2) / blob.substr(2);
	// Loose objects are read-only; Windows does not delete a read-only file.
	fs::permissions(object, fs::perms::owner_write, fs::perm_options::add, ec);
	BOOST_REQUIRE_MESSAGE(fs::remove(object, ec), ec.message());
	git(repo, { "config", "core.repositoryformatversion", "1" });
	git(repo, { "remote", "add", "origin", "https://127.0.0.1:9/unreachable.git" });
	git(repo, { "config", "extensions.partialClone", "origin" });
	const fs::path marker = base / "partial_helper_ran.txt";
	const fs::path script = base / "partial-helper.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\n");
	git(repo, { "config", "credential.helper", generic_text(script) });
	const fs::path trace = base / "partial_trace.txt";
	auto options = real_options(repo);
	edt::set_variable(*options.environment, "GIT_TRACE", edt::utf8_of(trace));
	make_service(std::move(options));

	const json::value refused = finish(service->start_checkout("other", cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("extensions.partialclone") != std::string::npos);

	fs::remove(trace, ec);
	service->set_write_permissions({ false, false, false, true });
	if (!git_at_least(service->info().git_version, 2, 45)) {
		const json::value old_git = finish(service->start_checkout("other", cmd::origin_kind::agent));
		BOOST_TEST_INFO("old git: " << json::serialize(old_git));
		BOOST_REQUIRE(is_error(old_git, "unsafe_repository"));
		BOOST_TEST(text(old_git.as_object().at("error").as_object().at("message")).find("lazy fetch") != std::string::npos);
		BOOST_TEST(!fs::exists(marker, ec));
		return;
	}
	// Git without lazy fetch cannot write big.txt: the checkout fails in git (git_failed), or the policy refuses first.
	const json::value no_fetch = finish(service->start_checkout("other", cmd::origin_kind::agent));
	BOOST_TEST((is_error(no_fetch, "git_failed") || is_error(no_fetch, "unsafe_repository")), json::serialize(no_fetch));
	std::string traced;
	{
		std::ifstream in(trace, std::ios::binary);
		traced.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	BOOST_TEST(!traced.empty(), "GIT_TRACE wrote nothing: the check below proves nothing");
	BOOST_TEST(traced.find("fetch") == std::string::npos, traced);
	BOOST_TEST(!fs::exists(marker, ec));
}

// Second review of #474 (N3): check-attr reads the work tree's .gitattributes first, but checkout smudges with the
// index's. A work tree .gitattributes that drops the filter line must not let the checkout through.
BOOST_AUTO_TEST_CASE(AWorkTreeGitattributesThatHidesAFilter_DoesNotPassTheCheck)
{
	isolate_config();
	const fs::path repo = make_plain_repository("hidden_attributes");
	write(repo / ".gitattributes", "*.bin filter=evil\n");
	write(repo / "data.bin", "raw bytes\n");
	git(repo, { "add", ".gitattributes", "data.bin" });
	git(repo, { "commit", "-q", "-m", "attributes" });
	git(repo, { "checkout", "-q", "-b", "other" });
	write(repo / "data.bin", "other bytes\n");
	git(repo, { "add", "data.bin" });
	git(repo, { "commit", "-q", "-m", "other bytes" });
	git(repo, { "checkout", "-q", "main" });
	const fs::path marker = base / "hidden_filter_ran.txt";
	const fs::path script = base / "hidden-filter.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\ncat\n");
	git(repo, { "config", "filter.evil.clean", generic_text(script) });
	git(repo, { "config", "filter.evil.smudge", generic_text(script) });
	write(repo / ".gitattributes", "");
	make_service(real_options(repo));

	const json::value refused = finish(service->start_checkout("other", cmd::origin_kind::agent));
	BOOST_TEST(is_error(refused, "filters_require_opt_in"), json::serialize(refused));
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
	std::string head_ref = git_text(repo, { "symbolic-ref", "HEAD" });
	while (!head_ref.empty() && (head_ref.back() == '\n' || head_ref.back() == '\r')) head_ref.pop_back();
	BOOST_TEST(head_ref == "refs/heads/main");
}

// Final review of #474 (N2b): git 2.43's `clone --filter=blob:none` writes remote.origin.promisor=true and
// partialclonefilter, no extension, and a checkout lazy-fetches. Refused by the allowlist; with
// allow_any_repo_config, refused before git 2.45 and fetching nothing (GIT_TRACE) from it.
BOOST_AUTO_TEST_CASE(APromisorRemoteWithoutTheExtension_LazyFetchesNothingOnAGuardedCheckout)
{
	isolate_config();
	const fs::path repo = make_plain_repository("promisor");
	git(repo, { "checkout", "-q", "-b", "other" });
	write(repo / "big.txt", "a blob the promisor would have to send\n");
	git(repo, { "add", "big.txt" });
	git(repo, { "commit", "-q", "-m", "big" });
	git(repo, { "checkout", "-q", "main" });
	std::string blob = git_text(repo, { "rev-parse", "other:big.txt" });
	while (!blob.empty() && (blob.back() == '\n' || blob.back() == '\r')) blob.pop_back();
	BOOST_REQUIRE(blob.size() > 2);
	std::error_code ec;
	const fs::path object = repo / ".git" / "objects" / blob.substr(0, 2) / blob.substr(2);
	fs::permissions(object, fs::perms::owner_write, fs::perm_options::add, ec);
	BOOST_REQUIRE_MESSAGE(fs::remove(object, ec), ec.message());
	git(repo, { "remote", "add", "origin", "https://127.0.0.1:9/unreachable.git" });
	git(repo, { "config", "remote.origin.promisor", "true" });
	git(repo, { "config", "remote.origin.partialclonefilter", "blob:none" });
	const fs::path marker = base / "promisor_helper_ran.txt";
	const fs::path script = base / "promisor-helper.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\n");
	git(repo, { "config", "credential.helper", generic_text(script) });
	const fs::path trace = base / "promisor_trace.txt";
	auto options = real_options(repo);
	edt::set_variable(*options.environment, "GIT_TRACE", edt::utf8_of(trace));
	make_service(std::move(options));

	const json::value refused = finish(service->start_checkout("other", cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("remote.origin.promisor") != std::string::npos);

	fs::remove(trace, ec);
	service->set_write_permissions({ false, false, false, true });
	const json::value opted_in = finish(service->start_checkout("other", cmd::origin_kind::agent));
	BOOST_TEST_INFO("opted in: " << json::serialize(opted_in));
	if (!git_at_least(service->info().git_version, 2, 45)) {
		BOOST_REQUIRE(is_error(opted_in, "unsafe_repository"));
		BOOST_TEST(text(opted_in.as_object().at("error").as_object().at("message")).find("remote.origin.promisor") != std::string::npos);
	}
	else {
		BOOST_TEST((is_error(opted_in, "git_failed") || is_error(opted_in, "unsafe_repository")));
		std::string traced;
		{
			std::ifstream in(trace, std::ios::binary);
			traced.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		BOOST_TEST(!traced.empty(), "GIT_TRACE wrote nothing: the check below proves nothing");
		BOOST_TEST(traced.find("fetch") == std::string::npos, traced);
	}
	BOOST_TEST(!fs::exists(marker, ec));
}

// Final review of #474 (N3b): an untracked top-level .gitattributes sets the filter, a tracked sub/.gitattributes
// edited in the work tree unsets it: none of the three check-attr views names the driver, yet checkout smudges. A
// tracked .gitattributes that differs from the index refuses on every git.
BOOST_AUTO_TEST_CASE(AMixedAttributesLayout_DoesNotHideAFilter)
{
	isolate_config();
	const fs::path repo = make_plain_repository("mixed_attributes");
	write(repo / "sub" / ".gitattributes", "*.txt text\n");
	write(repo / "sub" / "f.txt", "one\n");
	git(repo, { "add", "sub/.gitattributes", "sub/f.txt" });
	git(repo, { "commit", "-q", "-m", "sub" });
	git(repo, { "checkout", "-q", "-b", "other" });
	write(repo / "sub" / "f.txt", "two\n");
	git(repo, { "add", "sub/f.txt" });
	git(repo, { "commit", "-q", "-m", "change f" });
	git(repo, { "checkout", "-q", "main" });
	const fs::path marker = base / "mixed_filter_ran.txt";
	const fs::path script = base / "mixed-filter.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\ncat\n");
	git(repo, { "config", "filter.evil.clean", generic_text(script) });
	git(repo, { "config", "filter.evil.smudge", generic_text(script) });
	// Only sub/ (a.txt at the top would show the driver to the work-tree view).
	write(repo / ".gitattributes", "sub/*.txt filter=evil\n");
	write(repo / "sub" / ".gitattributes", "*.txt -filter\n");
	make_service(real_options(repo));

	const json::value refused = finish(service->start_checkout("other", cmd::origin_kind::agent));
	BOOST_TEST(is_error(refused, "filters_require_opt_in"), json::serialize(refused));
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
}

// Final review of #474 (hardening): a repository's protocol.file.allow=always beats protocol.allow=never; the
// specific -c protocol.file.allow=never beats it. With allow_any_repo_config the push to a local repository is still
// refused by git, and that repository's pre-receive does not run.
BOOST_AUTO_TEST_CASE(ARepositoryProtocolFileAllow_DoesNotOpenTheFileTransport)
{
	isolate_config();
	const fs::path remote = make_bare("file_allowed_remote.git");
	const fs::path marker = base / "file_allowed_pre_receive_ran.txt";
	make_script(remote / "hooks" / "pre-receive", "echo ran >> '" + generic_text(marker) + "'\nexit 0\n");
	const fs::path repo = make_plain_repository("file_allowed");
	git(repo, { "remote", "add", "origin", file_url(remote) });
	git(repo, { "config", "protocol.file.allow", "always" });
	git(repo, { "branch", "agent/x" });
	make_service(real_options(repo));
	service->set_write_permissions({ false, false, false, true });

	const json::value refused = finish(service->start_push("origin", "agent/x", false, cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_TEST(refused.as_object().contains("error"));
	BOOST_TEST(!is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("stderr_tail")).find("not allowed") != std::string::npos);
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(git_text(remote, { "for-each-ref" }).empty());
}

// Final review of #474 (hardening): config-based hooks (hook.<name>.command/.event) do not go through
// core.hooksPath. Without allow_hooks any hook.* key refuses, even with allow_any_repo_config.
BOOST_AUTO_TEST_CASE(AConfigBasedHook_RefusesAGuardedWrite_EvenWithAnyRepoConfig)
{
	isolate_config();
	const fs::path repo = make_plain_repository("config_hook");
	const fs::path marker = base / "config_hook_ran.txt";
	const fs::path script = base / "config-hook.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\nexit 0\n");
	git(repo, { "config", "hook.evil.command", generic_text(script) });
	git(repo, { "config", "hook.evil.event", "pre-commit" });
	write(repo / "a.txt", "changed\n");
	git(repo, { "add", "a.txt" });
	const std::string before = head_of(repo);
	make_service(real_options(repo));
	service->set_write_permissions({ false, false, false, true });

	const json::value refused = finish(service->start_commit("message", cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("hook.evil.command") != std::string::npos);
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(head_of(repo) == before);
}

// Re-check of #474: `config --local --list` does not follow include.path, git does. An included hook.* must not pass.
BOOST_AUTO_TEST_CASE(AnIncludedConfigFile_RefusesAGuardedWrite_EvenWithAnyRepoConfig)
{
	isolate_config();
	const fs::path repo = make_plain_repository("included_hook");
	const fs::path marker = base / "included_hook_ran.txt";
	const fs::path script = base / "included-hook.sh";
	make_script(script, "echo ran >> '" + generic_text(marker) + "'\nexit 0\n");
	const fs::path included = base / "included_hook.inc";
	write(included, "[hook \"evil\"]\n\tcommand = " + generic_text(script) + "\n\tevent = pre-commit\n");
	git(repo, { "config", "include.path", generic_text(included) });
	write(repo / "a.txt", "changed\n");
	git(repo, { "add", "a.txt" });
	const std::string before = head_of(repo);
	make_service(real_options(repo));
	service->set_write_permissions({ false, false, false, true });

	const json::value refused = finish(service->start_commit("message", cmd::origin_kind::agent));
	BOOST_TEST_INFO("refused: " << json::serialize(refused));
	BOOST_REQUIRE(is_error(refused, "unsafe_repository"));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find("include.path") != std::string::npos);
	std::error_code ec;
	BOOST_TEST(!fs::exists(marker, ec));
	BOOST_TEST(head_of(repo) == before);
}

// #455: the policy source decides, per write, whether an agent's commit runs the repository's hooks; it replaces
// set_write_permissions.
BOOST_AUTO_TEST_CASE(APolicySource_DecidesWhetherAnAgentsCommitRunsTheHook)
{
	isolate_config();
	const fs::path repo = make_plain_repository("policy_hooks");
	const fs::path marker = base / "policy_hook_ran.txt";
	make_script(repo / ".git" / "hooks" / "pre-commit", "echo ran >> '" + generic_text(marker) + "'\nexit 0\n");
	make_service(real_options(repo));
	bool allow_hooks = false;
	service->set_policy_source([&allow_hooks] {
		edt::vcs_policy policy;
		policy.permissions.allow_hooks = allow_hooks;
		return policy;
	});
	// Ignored while a source is set.
	service->set_write_permissions({ true, false, false, false });
	std::error_code ec;

	write(repo / "a.txt", "hooks off\n");
	finish(service->start_stage({ "a.txt" }, cmd::origin_kind::ui));
	const json::value off = finish(service->start_commit("hooks off", cmd::origin_kind::agent));
	BOOST_REQUIRE_MESSAGE(off.as_object().contains("hash"), json::serialize(off));
	BOOST_TEST(!fs::exists(marker, ec), "the source said no hooks");

	allow_hooks = true;
	write(repo / "a.txt", "hooks on\n");
	finish(service->start_stage({ "a.txt" }, cmd::origin_kind::ui));
	const json::value on = finish(service->start_commit("hooks on", cmd::origin_kind::agent));
	BOOST_REQUIRE_MESSAGE(on.as_object().contains("hash"), json::serialize(on));
	BOOST_TEST(fs::exists(marker, ec), "the source allowed hooks: the pre-commit hook must run");
}

// #455: a guarded commit of a valid agent session ends with an Agent-Session trailer; no other commit gets one.
BOOST_AUTO_TEST_CASE(AnAgentsCommit_CarriesItsSessionAsATrailer)
{
	isolate_config();
	const fs::path repo = make_plain_repository("trailer");
	make_service(real_options(repo));
	int round = 0;
	const auto commit_body = [this, &repo, &round](const std::string& message, const cmd::origin& caller) {
		write(repo / "a.txt", std::format("round {}\n", ++round));
		finish(service->start_stage({ "a.txt" }, cmd::origin_kind::ui));
		const json::value committed = finish(service->start_commit(message, caller));
		BOOST_REQUIRE_MESSAGE(committed.as_object().contains("hash"), json::serialize(committed));
		std::string body = git_text(repo, { "log", "-1", "--format=%B" });
		while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
		return body;
	};
	const std::string session = "0123456789abcdef0123456789abcdef";

	const std::string tagged = commit_body("Agent change\n\nWhy it was made.\n", cmd::origin::agent(session));
	BOOST_TEST(tagged.ends_with("\n\nAgent-Session: " + session), tagged);
	BOOST_TEST(tagged.starts_with("Agent change\n\nWhy it was made."), tagged);

	// A forged trailer (any case, any spacing) is dropped and the real one added.
	const std::string forged = commit_body("Forged\n\nagent-session : evil1\nAGENT-SESSION:evil2\nKeep: me\n", cmd::origin::agent(session));
	BOOST_TEST(forged.ends_with("\n\nAgent-Session: " + session), forged);
	BOOST_TEST(forged.find("evil") == std::string::npos, forged);
	BOOST_TEST(forged.find("Keep: me") != std::string::npos, forged);
	// With an invalid session the forged line goes too, and nothing is added.
	const std::string no_session = commit_body("Forged\n\nAgent-Session: abc\n", cmd::origin::agent("NOT-HEX"));
	BOOST_TEST(no_session.find("Agent-Session") == std::string::npos, no_session);
	BOOST_TEST(service->start_commit("Agent-Session: abc\n", cmd::origin::agent(session)).code == cmd::codes::invalid_args);

	for (const std::string& bad : std::vector<std::string>{ "0123abcd\nAgent-Session: evil", "0123ABCD", "s1", "", std::string(65, 'a') }) {
		BOOST_TEST_CONTEXT(bad) {
			const std::string body = commit_body("No trailer", cmd::origin::agent(bad));
			BOOST_TEST(body.find("Agent-Session") == std::string::npos, body);
		}
	}
	const std::string by_person = commit_body("By a person", cmd::origin{ cmd::origin_kind::ui, session });
	BOOST_TEST(by_person.find("Agent-Session") == std::string::npos, by_person);
}

// Security review of #481: an agent must not change the policy that judges it. A guarded checkout whose target changes
// the project's .snk/agent_policy.json is denied before git runs; one that leaves the file alone goes through.
BOOST_AUTO_TEST_CASE(AGuardedCheckout_ThatChangesTheAgentPolicyFile_IsDenied)
{
	isolate_config();
	const fs::path repo = make_plain_repository("policy_checkout");
	const fs::path policy_file = repo / ".snk" / "agent_policy.json";
	write(policy_file, "{}\n");
	git(repo, { "add", ".snk/agent_policy.json" });
	git(repo, { "commit", "-q", "-m", "a weak policy" });
	const std::string weak = head_of(repo);
	const std::string strict = R"({"vcs":{"remotes":[]}})" "\n";
	write(policy_file, strict);
	git(repo, { "commit", "-q", "-am", "a strict policy" });
	const std::string head = head_of(repo);
	git(repo, { "checkout", "-q", "-b", "side" });
	write(repo / "other.txt", "other\n");
	git(repo, { "add", "other.txt" });
	git(repo, { "commit", "-q", "-m", "not the policy" });
	git(repo, { "checkout", "-q", "main" });
	make_service(real_options(repo));
	const auto read_policy = [&policy_file] {
		std::ifstream in(policy_file, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	};

	const json::value refused = finish(service->start_checkout(weak, cmd::origin_kind::agent));
	BOOST_TEST(is_error(refused, "denied"), json::serialize(refused));
	BOOST_TEST(text(refused.as_object().at("error").as_object().at("message")).find(".snk/agent_policy.json") != std::string::npos);
	BOOST_TEST(read_policy() == strict);
	BOOST_TEST(head_of(repo) == head);

	const json::value allowed = finish(service->start_checkout("side", cmd::origin_kind::agent));
	BOOST_TEST(!allowed.as_object().contains("error"), json::serialize(allowed));
	BOOST_TEST(read_policy() == strict);
	BOOST_TEST(fs::exists(repo / "other.txt"));
}

// The same with the project in a subfolder of the repository: the project's game/.snk/agent_policy.json is the one
// checked, not a .snk at the repository's top.
BOOST_AUTO_TEST_CASE(AGuardedCheckout_ChecksThePolicyFileOfAProjectInASubfolder)
{
	isolate_config();
	const fs::path repo = make_plain_repository("policy_subfolder");
	const fs::path project = repo / "game";
	write(project / ".snk" / "agent_policy.json", "{}\n");
	git(repo, { "add", "game/.snk/agent_policy.json" });
	git(repo, { "commit", "-q", "-m", "a weak policy" });
	const std::string weak = head_of(repo);
	write(project / ".snk" / "agent_policy.json", R"({"vcs":{"remotes":[]}})" "\n");
	git(repo, { "commit", "-q", "-am", "a strict policy" });
	git(repo, { "checkout", "-q", "-b", "side" });
	write(repo / ".snk" / "agent_policy.json", "{}\n");
	git(repo, { "add", ".snk/agent_policy.json" });
	git(repo, { "commit", "-q", "-m", "a file of the same name outside the project" });
	git(repo, { "checkout", "-q", "main" });
	make_service(real_options(project));

	BOOST_TEST(is_error(finish(service->start_checkout(weak, cmd::origin_kind::agent)), "denied"));
	const json::value allowed = finish(service->start_checkout("side", cmd::origin_kind::agent));
	BOOST_TEST(!allowed.as_object().contains("error"), json::serialize(allowed));
}

// Security review of #481: with no remote named, the remote checked is the one git would use (the branch's), and
// a local URL rewrite under allow_any_repo_config changes nothing about that.
BOOST_AUTO_TEST_CASE(AnAgentsPull_WithoutARemote_IsCheckedAgainstTheBranchsRemote)
{
	isolate_config();
	const fs::path repo = make_plain_repository("branch_remote");
	git(repo, { "remote", "add", "origin", "https://example.invalid/origin.git" });
	git(repo, { "remote", "add", "upstream", "https://example.invalid/upstream.git" });
	git(repo, { "config", "branch.main.remote", "upstream" });
	git(repo, { "config", "branch.main.merge", "refs/heads/main" });
	git(repo, { "config", "url.https://mirror.example.invalid/.insteadOf", "https://example.invalid/" });
	make_service(real_options(repo));
	service->set_write_permissions({ false, false, false, true });

	BOOST_TEST(is_error(finish(service->start_pull({}, {}, cmd::origin_kind::agent)), "denied"));
	BOOST_TEST(is_error(finish(service->start_fetch({}, cmd::origin_kind::agent)), "denied"));
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

// git checkout can report a file it could not write and still exit 0 (a partial clone without lazy fetch).
BOOST_AUTO_TEST_CASE(ACheckoutThatReportsAnErrorButExitsZero_IsAFailure)
{
	command = [](const std::vector<std::string>& args, const platform::process_options& opts,
		const std::function<void(std::string_view)>&, std::stop_token) {
		if (std::find(args.begin(), args.end(), "checkout") != args.end()) {
			opts.on_stderr_line("error: unable to read sha1 file of big.txt (0123456789abcdef0123456789abcdef01234567)");
		}
		return exited(0);
	};
	make_service(fake_options());

	const json::value result = finish(service->start_checkout("other", cmd::origin_kind::ui));
	BOOST_TEST_INFO("result: " << json::serialize(result));
	BOOST_REQUIRE(is_error(result, "git_failed"));
	BOOST_TEST(text(result.as_object().at("error").as_object().at("message")).find("unable to read sha1 file")
		!= std::string::npos);
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

// A log answer of `count` commits, as `git log -z <k_log_format>` writes them.
std::string fake_log(int count)
{
	std::string out;
	for (int i = 1; i <= count; ++i) out += std::format("{:040x}", i) + "\0\0A\0a@x\0" "5\0s\0\0"s;
	return out;
}

const std::vector<std::string>* last_call_with(const std::vector<std::vector<std::string>>& calls, std::string_view arg)
{
	for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
		if (has_arg(*it, arg)) return &*it;
	}
	return nullptr;
}

// The first page resolves the ref once (rev-parse --verify --end-of-options <ref>^{commit}) and logs from that commit;
// a later page logs from the snapshot's commit without resolving again.
BOOST_AUTO_TEST_CASE(Log_PassesThePageAndClampsTheLimit)
{
	const std::string oid(40, 'a');
	command = [&oid](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "rev-parse")) on_line(oid + "\n");
		if (has_arg(args, "log")) on_line(fake_log(201));
		return exited(0);
	};
	make_service(fake_options());

	const json::value page = finish(service->start_log("main", "src/a b.cpp", 500, {}));
	BOOST_TEST(page.as_object().at("commits").as_array().size() == 200u);
	BOOST_REQUIRE(page.as_object().contains("next_cursor"));
	const std::string cursor = text(page.as_object().at("next_cursor"));
	BOOST_TEST(cursor == "s1:200");
	const auto* resolve = last_call_with(calls, "rev-parse");
	BOOST_REQUIRE(resolve != nullptr);
	BOOST_TEST(has_arg(*resolve, "--verify"));
	BOOST_REQUIRE(resolve->size() >= 2u);
	BOOST_TEST((*resolve)[resolve->size() - 2] == "--end-of-options");
	BOOST_TEST(resolve->back() == "main^{commit}");
	const auto* log = last_call_with(calls, "log");
	BOOST_REQUIRE(log != nullptr);
	const auto& args = *log;
	BOOST_TEST(has_arg(args, "--skip=0"));
	BOOST_TEST(has_arg(args, "--max-count=201"));
	BOOST_TEST(has_arg(args, "--date-order"));
	BOOST_TEST(has_arg(args, "--decorate=full"));
	BOOST_TEST(has_arg(args, "-z"));
	BOOST_TEST(!has_arg(args, "main"));
	BOOST_TEST(!has_arg(args, "--fixed-strings"));
	BOOST_REQUIRE(args.size() >= 4u);
	BOOST_TEST(args[args.size() - 4] == "--end-of-options");
	BOOST_TEST(args[args.size() - 3] == oid);
	BOOST_TEST(args[args.size() - 2] == "--");
	BOOST_TEST(args.back() == "src/a b.cpp");
	BOOST_TEST(std::any_of(args.begin(), args.end(), [](const std::string& a) { return a.starts_with("core.hooksPath="); }));
	BOOST_TEST(has_arg(args, "filter.lfs.process="));

	const auto resolved = std::count_if(calls.begin(), calls.end(), [](const auto& c) { return has_arg(c, "rev-parse"); });
	const json::value next = finish(service->start_log("main", "src/a b.cpp", 500, cursor));
	BOOST_TEST(text(next.as_object().at("next_cursor")) == "s1:400");
	BOOST_TEST(has_arg(*last_call_with(calls, "log"), "--skip=200"));
	BOOST_TEST(last_call_with(calls, "log")->back() == "src/a b.cpp");
	finish(service->start_log("main", "src/a b.cpp", 10, "s1:99999999999999999999999"));
	BOOST_TEST(has_arg(*last_call_with(calls, "log"), "--skip=1000000000"));
	BOOST_TEST(std::count_if(calls.begin(), calls.end(), [](const auto& c) { return has_arg(c, "rev-parse"); }) == resolved);

	// HEAD by default, and no path: nothing after "--".
	finish(service->start_log({}, {}, 10, {}));
	BOOST_TEST(last_call_with(calls, "rev-parse")->back() == "HEAD^{commit}");
	BOOST_TEST(last_call_with(calls, "log")->back() == "--");

	// A ref or cursor that looks like an option never reaches git.
	const std::size_t before = calls.size();
	BOOST_TEST(service->start_log("-p", {}, 10, {}).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_log("--output=/tmp/x", {}, 10, {}).code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_log({}, {}, 10, "-1").code == cmd::codes::invalid_args);
	BOOST_TEST(service->start_log({}, {}, 10, "40").code == cmd::codes::invalid_args);
	run_to_end();
	BOOST_TEST(calls.size() == before);
}

BOOST_AUTO_TEST_CASE(Log_AuthorAndGrep_AreLiteralCaseInsensitiveOptions)
{
	command = [](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "rev-parse")) on_line(std::string(40, 'b') + "\n");
		return exited(0);
	};
	make_service(fake_options());

	edt::vcs_service::log_filter filter;
	filter.author = "Ann --all";
	filter.grep = "a.b*";
	finish(service->start_log({}, {}, 10, {}, filter));
	const auto& args = *last_call_with(calls, "log");
	BOOST_TEST(has_arg(args, "--author=Ann --all"));
	BOOST_TEST(has_arg(args, "--grep=a.b*"));
	BOOST_TEST(has_arg(args, "--fixed-strings"));
	BOOST_TEST(has_arg(args, "-i"));
	const auto options_end = std::find(args.begin(), args.end(), "--end-of-options");
	BOOST_TEST((std::find(args.begin(), args.end(), "--fixed-strings") < options_end));
	BOOST_TEST((std::find(args.begin(), args.end(), "--grep=a.b*") < options_end));
}

// all: the commits of every branch, remote branch and tag (a tag peeled), once each; too many for a command line are
// cut down to the independent ones with merge-base --independent.
BOOST_AUTO_TEST_CASE(Log_All_PassesTheTipCommits_AndReducesTooManyToIndependentOnes)
{
	std::size_t tip_count = 3;
	command = [&tip_count](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "for-each-ref")) {
			std::string out;
			for (std::size_t i = 1; i <= tip_count; ++i) out += std::format("{:040x} commit  \n", i);
			out += std::format("{:040x} tag {:040x} commit\n", 9999, 1);
			on_line(out);
		}
		if (has_arg(args, "merge-base")) {
			const auto independent = std::find(args.begin(), args.end(), "--independent");
			on_line(*(independent + 1) + "\n");
		}
		return exited(0);
	};
	make_service(fake_options());

	edt::vcs_service::log_filter all;
	all.all = true;
	finish(service->start_log({}, {}, 10, {}, all));
	const auto* listed = last_call_with(calls, "for-each-ref");
	BOOST_REQUIRE(listed != nullptr);
	BOOST_TEST(has_arg(*listed, "refs/heads"));
	BOOST_TEST(has_arg(*listed, "refs/remotes"));
	BOOST_TEST(has_arg(*listed, "refs/tags"));
	BOOST_TEST(last_call_with(calls, "--verify") == nullptr);
	const auto& args = *last_call_with(calls, "log");
	const auto tips_begin = std::find(args.begin(), args.end(), "--end-of-options") + 1;
	const auto tips_end = std::find(tips_begin, args.end(), "--");
	BOOST_TEST((std::vector<std::string>(tips_begin, tips_end)
		== std::vector<std::string>{ std::format("{:040x}", 1), std::format("{:040x}", 2), std::format("{:040x}", 3) }));
	BOOST_TEST(!has_arg(args, "--branches"));

	tip_count = 600;
	finish(service->start_log({}, {}, 10, {}, all));
	std::size_t reductions = 0;
	for (const auto& call : calls) {
		if (!has_arg(call, "merge-base")) continue;
		++reductions;
		BOOST_TEST(call.size() <= 300u);
	}
	BOOST_TEST(reductions == 3u);
	const auto& reduced = *last_call_with(calls, "log");
	const auto reduced_begin = std::find(reduced.begin(), reduced.end(), "--end-of-options") + 1;
	BOOST_TEST(std::find(reduced_begin, reduced.end(), "--") - reduced_begin == 3);
}

// show_files and diff{commit}: the commit resolved (rev-parse --verify --end-of-options <x>^{commit}), its first parent
// (<oid>^1), then diff-tree against that parent with the read hardening; a root commit with --root.
BOOST_AUTO_TEST_CASE(ShowFilesAndCommitDiff_RunDiffTreeAgainstTheFirstParent)
{
	const std::string oid(40, 'c');
	const std::string parent(40, 'd');
	bool root = false;
	command = [&](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "rev-parse")) {
			if (args.back().ends_with("^1")) {
				if (root) return exited(1);
				on_line(parent + "\n");
			} else {
				on_line(oid + "\n");
			}
		}
		if (has_arg(args, "diff-tree") && has_arg(args, "--name-status")) on_line("R100\0old\0new\0M\0x\0"s);
		return exited(0);
	};
	make_service(fake_options());

	const json::value files = finish(service->start_show_files("cafe"));
	BOOST_TEST_INFO("files: " << json::serialize(files));
	const json::array& listed = files.as_object().at("files").as_array();
	BOOST_REQUIRE_EQUAL(listed.size(), 2u);
	BOOST_TEST(text(listed[0].as_object().at("orig_path")) == "old");
	const auto* tree = last_call_with(calls, "diff-tree");
	BOOST_REQUIRE(tree != nullptr);
	for (const char* arg : { "-r", "-z", "--name-status", "-M", "--no-ext-diff", "--no-textconv", "--no-commit-id",
			"filter.lfs.smudge=" }) {
		BOOST_TEST(has_arg(*tree, arg), arg);
	}
	BOOST_TEST(!has_arg(*tree, "--root"));
	BOOST_REQUIRE(tree->size() >= 2u);
	BOOST_TEST((*tree)[tree->size() - 2] == parent);
	BOOST_TEST(tree->back() == oid);
	BOOST_TEST(std::count_if(calls.begin(), calls.end(), [](const auto& c) { return has_arg(c, "cafe^{commit}"); }) == 1);

	root = true;
	finish(service->start_commit_diff("cafe", "dir/f.txt"));
	const auto& diff = *last_call_with(calls, "diff-tree");
	for (const char* arg : { "-p", "--no-color", "--no-ext-diff", "--no-textconv", "-M", "--root", "--no-commit-id" }) {
		BOOST_TEST(has_arg(diff, arg), arg);
	}
	BOOST_TEST(!has_arg(diff, parent));
	BOOST_TEST(diff[diff.size() - 3] == oid);
	BOOST_TEST(diff[diff.size() - 2] == "--");
	BOOST_TEST(diff.back() == "dir/f.txt");
	BOOST_TEST(std::any_of(diff.begin(), diff.end(), [](const std::string& a) { return a.starts_with("core.hooksPath="); }));
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
			"protocol.ssh.allow=always", "protocol.ext.allow=never", "protocol.file.allow=never", "protocol.git.allow=never", "commit.gpgsign=false", "tag.gpgsign=false",
			"merge.verifySignatures=false", "gpg.program=", "gpg.openpgp.program=", "gpg.x509.program=", "gpg.ssh.program=",
			"gpg.ssh.defaultKeyCommand=", "core.gitProxy=", "core.alternateRefsCommand=", "gc.auto=0",
			"maintenance.auto=false",
			"remote.origin.uploadpack=git-upload-pack", "remote.origin.receivepack=git-receive-pack" }) {
		BOOST_TEST_CONTEXT(config) {
			BOOST_TEST(has_arg(allowed, config));
		}
	}
	BOOST_TEST(edt::variable_of(*last_options.environment, "GIT_PROXY_COMMAND").value_or("x").empty());
	BOOST_TEST(edt::variable_of(*last_options.environment, "GIT_NO_LAZY_FETCH").value_or("") == "1");
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
	command = [](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "symbolic-ref")) on_line("main\n");
		if (has_arg(args, "branch.main.merge")) on_line("refs/heads/main\n");
		return exited(0);
	};
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
	// The remote and the upstream, resolved once, are named to git.
	const auto fetch = std::find_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) { return has_arg(call, "fetch"); });
	BOOST_REQUIRE(fetch != calls.end());
	BOOST_REQUIRE(fetch->size() >= 3u);
	BOOST_TEST((*fetch)[fetch->size() - 3] == "--end-of-options");
	BOOST_TEST((*fetch)[fetch->size() - 2] == "origin");
	BOOST_TEST(fetch->back() == "refs/heads/main");
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
		if (has_arg(args, "symbolic-ref")) {
			on_line("main\n");
			return exited(0);
		}
		if (has_arg(args, "branch.main.merge")) {
			on_line("refs/heads/main\n");
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
	finish(service->start_push("origin", "agent/topic", true, agent));
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
		return finish(service->start_push("origin", "agent/x", false, caller));
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
			service->start_push("origin", "agent/x", false, agent) }) {
		BOOST_TEST(started.code == "vcs_unavailable");
		BOOST_TEST(started.error == expected);
	}
	BOOST_TEST(jobs->list().empty());
	// A person's network call and an agent's local write still run.
	BOOST_TEST(!finish(service->start_push("origin", "main", false, cmd::origin_kind::ui)).as_object().contains("error"));
	BOOST_TEST(!finish(service->start_branch_create("topic", {}, agent)).as_object().contains("error"));
}

// #455, required: with the default policy (every vcs_* category allowed) an agent pushes only to agent/*, never to
// main or master in any case, nor to a protected branch the project names; the refusal comes before git runs.
BOOST_AUTO_TEST_CASE(AnAgentsPush_OutsideThePolicysBranches_IsDeniedBeforeGitRuns)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	make_service(fake_options());
	const auto pushes = [this] {
		return std::count_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) { return has_arg(call, "push"); });
	};
	const auto agent = cmd::origin::agent("0123456789abcdef0123456789abcdef");

	for (const char* branch : { "feature/x", "main", "MAIN", "master", "Master", "agentx" }) {
		BOOST_TEST_CONTEXT(branch) {
			const json::value result = finish(service->start_push("origin", branch, false, agent));
			BOOST_TEST(is_error(result, "denied"), json::serialize(result));
		}
	}
	BOOST_TEST(pushes() == 0);
	BOOST_TEST(!finish(service->start_push("origin", "agent/x", false, agent)).as_object().contains("error"));
	BOOST_TEST(pushes() == 1);

	// A person's push is not held by these rules.
	BOOST_TEST(!finish(service->start_push("origin", "main", false, cmd::origin_kind::ui)).as_object().contains("error"));
	BOOST_TEST(pushes() == 2);

	// The project allows any branch but protects release/*: protected wins; main and master stay protected.
	service->set_policy_source([] {
		edt::vcs_policy policy;
		policy.push_branches = { "*" };
		policy.protected_branches = { "main", "master", "release/*" };
		return policy;
	});
	for (const char* branch : { "release/1.0", "Release/2", "main", "master" }) {
		BOOST_TEST_CONTEXT(branch) {
			BOOST_TEST(is_error(finish(service->start_push("origin", branch, false, agent)), "denied"));
		}
	}
	BOOST_TEST(pushes() == 2);
	BOOST_TEST(!finish(service->start_push("origin", "feature/x", false, agent)).as_object().contains("error"));
	BOOST_TEST(pushes() == 3);
}

// #455: fetch, pull and push of an agent reach only the remotes of the policy, the one git would pick included.
BOOST_AUTO_TEST_CASE(AnAgentsNetworkCall_ToARemoteOutsideThePolicy_IsDenied)
{
	std::string branch_remote = "upstream";
	command = [&branch_remote](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "symbolic-ref")) {
			on_line("agent/x\n");
			return exited(0);
		}
		if (has_arg(args, "--get") && has_arg(args, "branch.agent/x.remote")) {
			on_line(branch_remote + "\n");
			return exited(0);
		}
		return exited(0);
	};
	write(base / "FETCH_HEAD", "1111111111111111111111111111111111111111\t\tbranch 'main' of /elsewhere\n");
	make_service(fake_options());
	const auto network = [this] {
		return std::count_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) {
			return has_arg(call, "fetch") || has_arg(call, "push");
		});
	};
	const auto agent = cmd::origin_kind::agent;

	BOOST_TEST(is_error(finish(service->start_push("upstream", "agent/x", false, agent)), "denied"));
	BOOST_TEST(is_error(finish(service->start_fetch("upstream", agent)), "denied"));
	BOOST_TEST(is_error(finish(service->start_pull("upstream", "main", agent)), "denied"));
	// No remote named: the current branch's (upstream) is the one checked.
	BOOST_TEST(is_error(finish(service->start_fetch({}, agent)), "denied"));
	BOOST_TEST(is_error(finish(service->start_pull({}, {}, agent)), "denied"));
	BOOST_TEST(network() == 0);

	branch_remote = "origin";
	BOOST_TEST(!finish(service->start_fetch({}, agent)).as_object().contains("error"));
	BOOST_TEST(network() == 1);

	// A person's network call is not held by them.
	BOOST_TEST(!finish(service->start_fetch("upstream", cmd::origin_kind::ui)).as_object().contains("error"));
	BOOST_TEST(!finish(service->start_push("upstream", "feature", false, cmd::origin_kind::ui)).as_object().contains("error"));
	BOOST_TEST(network() == 3);

	service->set_policy_source([] {
		edt::vcs_policy policy;
		policy.remotes = { "origin", "upstream" };
		return policy;
	});
	BOOST_TEST(!finish(service->start_push("upstream", "agent/x", false, agent)).as_object().contains("error"));
	BOOST_TEST(network() == 4);
}

// #455: an agent's push names its destination, refs/heads/<b>:refs/heads/<b>, without tags: an upstream of another
// branch (branch.<b>.merge), push.default or remote.*.push cannot send it to main.
BOOST_AUTO_TEST_CASE(AnAgentsPush_UsesAnExplicitRefspec_WhateverTheUpstreamSays)
{
	command = [](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "--get") && has_arg(args, "branch.agent/x.merge")) {
			on_line("refs/heads/main\n");
			return exited(0);
		}
		return exited(0);
	};
	make_service(fake_options());

	for (const bool set_upstream : { false, true }) {
		BOOST_TEST_CONTEXT("set_upstream " << set_upstream) {
			const json::value result = finish(service->start_push("origin", "agent/x", set_upstream, cmd::origin_kind::agent));
			BOOST_TEST(!result.as_object().contains("error"), json::serialize(result));
			const auto& push = calls.back();
			BOOST_REQUIRE(has_arg(push, "push"));
			BOOST_TEST(has_arg(push, "--no-follow-tags"));
			BOOST_TEST(has_arg(push, "--set-upstream") == set_upstream);
			BOOST_TEST(!has_arg(push, "--force"));
			BOOST_TEST(!has_arg(push, "--tags"));
			BOOST_REQUIRE(push.size() >= 3u);
			BOOST_TEST(push[push.size() - 3] == "--end-of-options");
			BOOST_TEST(push[push.size() - 2] == "origin");
			BOOST_TEST(push.back() == "refs/heads/agent/x:refs/heads/agent/x");
			BOOST_TEST(!has_arg(push, "main"));
			BOOST_TEST(!has_arg(push, "refs/heads/main"));
		}
	}
}

// #455: a guarded branch name follows git's ref name rules, checked before any job.
BOOST_AUTO_TEST_CASE(AGuardedBranchName_FollowsGitsRefNameRules)
{
	command = [](const std::vector<std::string>&, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) { return exited(0); };
	make_service(fake_options());
	const auto agent = cmd::origin_kind::agent;

	for (const char* name : { "agent/../main", "agent/x.lock", "-x", "agent/@{u}", "agent/a b", "agent/", "/agent/x", "agent//x",
			"agent/.x", "agent/x.", "agent/x~1", "agent/x^", "agent\\x", "agent/x\t", "@", "agent/x.lock/y" }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(service->start_push("origin", name, false, agent).code == cmd::codes::invalid_args);
			BOOST_TEST(service->start_branch_create(name, {}, agent).code == cmd::codes::invalid_args);
		}
	}
	BOOST_TEST(jobs->list().empty());
	for (const char* name : { "agent/x", "agent/feature-1.2", "agent/a.b/c_d" }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(!finish(service->start_branch_create(name, {}, agent)).as_object().contains("error"));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheVcsGlob)
{
	BOOST_TEST(edt::vcs_glob_match("agent/*", "agent/x"));
	BOOST_TEST(edt::vcs_glob_match("agent/*", "agent/a/b"));
	BOOST_TEST(!edt::vcs_glob_match("agent/*", "Agent/x"));
	BOOST_TEST(edt::vcs_glob_match("Agent/*", "agent/x", true));
	BOOST_TEST(!edt::vcs_glob_match("agent/*", "agentx"));
	BOOST_TEST(edt::vcs_glob_match("*", "anything/at/all"));
	BOOST_TEST(edt::vcs_glob_match("v?", "v1"));
	BOOST_TEST(!edt::vcs_glob_match("v?", "v10"));
	BOOST_TEST(edt::vcs_glob_match("main", "MAIN", true));
	BOOST_TEST(!edt::vcs_glob_match("main", "MAIN"));
	BOOST_TEST(!edt::vcs_glob_match("main", "main2"));
	BOOST_TEST(edt::vcs_glob_match("a*b*c", "a/x/b/y/c"));
	BOOST_TEST(!edt::vcs_glob_match("a*b*c", "a/x/b/y/d"));
}

// Security review of #481: a branch without a remote in a repository whose only remote is not origin: git uses that
// remote, so the check does too, and git is then told the remote (and the upstream) by name.
BOOST_AUTO_TEST_CASE(AnOmittedRemote_IsResolvedOnce_AsGitWould_AndNamedToGit)
{
	bool detached = false;
	command = [&detached](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "symbolic-ref")) {
			if (detached) return exited(1);
			on_line("main\n");
			return exited(0);
		}
		if (has_arg(args, "branch.main.remote")) return exited(1);
		if (has_arg(args, "branch.main.merge")) {
			on_line("refs/heads/main\n");
			return exited(0);
		}
		if (has_arg(args, "remote")) {
			on_line("upstream\n");
			return exited(0);
		}
		return exited(0);
	};
	write(base / "FETCH_HEAD", "1111111111111111111111111111111111111111\t\tbranch 'main' of /elsewhere\n");
	make_service(fake_options());
	const auto fetches = [this] {
		std::vector<std::vector<std::string>> out;
		for (const auto& call : calls) {
			if (has_arg(call, "fetch")) out.push_back(call);
		}
		return out;
	};
	const auto agent = cmd::origin_kind::agent;

	BOOST_TEST(is_error(finish(service->start_fetch({}, agent)), "denied"));
	BOOST_TEST(is_error(finish(service->start_pull({}, {}, agent)), "denied"));
	BOOST_TEST(fetches().empty());

	service->set_policy_source([] {
		edt::vcs_policy policy;
		policy.remotes = { "upstream" };
		return policy;
	});
	BOOST_TEST(!finish(service->start_fetch({}, agent)).as_object().contains("error"));
	BOOST_REQUIRE_EQUAL(fetches().size(), 1u);
	auto fetch = fetches().back();
	BOOST_REQUIRE(fetch.size() >= 2u);
	BOOST_TEST(fetch[fetch.size() - 2] == "--end-of-options");
	BOOST_TEST(fetch.back() == "upstream");

	finish(service->start_pull({}, {}, agent));
	BOOST_REQUIRE_EQUAL(fetches().size(), 2u);
	fetch = fetches().back();
	BOOST_REQUIRE(fetch.size() >= 3u);
	BOOST_TEST(fetch[fetch.size() - 3] == "--end-of-options");
	BOOST_TEST(fetch[fetch.size() - 2] == "upstream");
	BOOST_TEST(fetch.back() == "refs/heads/main");

	// Detached: no upstream to name, so no guess either.
	detached = true;
	const json::value refused = finish(service->start_pull({}, {}, agent));
	BOOST_TEST(is_error(refused, "git_failed"), json::serialize(refused));
	BOOST_TEST(fetches().size() == 2u);
}

// Security review of #481: a guarded pull whose fetched commit changes the policy file is denied before the merge;
// on a branch without commits, a target that holds the file is denied too.
BOOST_AUTO_TEST_CASE(AGuardedPull_ThatChangesTheAgentPolicyFile_IsDenied)
{
	write(base / "FETCH_HEAD", "1111111111111111111111111111111111111111\t\tbranch 'main' of /elsewhere\n");
	bool changes_policy = true;
	bool unborn = false;
	command = [&changes_policy](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "symbolic-ref")) on_line("main\n");
		if (has_arg(args, "branch.main.merge")) on_line("refs/heads/main\n");
		if (has_arg(args, "diff-tree")) return exited(changes_policy ? 1 : 0);
		return exited(0);
	};
	auto options = fake_options();
	options.run = [inner = options.run, &unborn](const fs::path& git, const std::vector<std::string>& args,
		const platform::process_options& opts, const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		if (has_arg(args, "1111111111111111111111111111111111111111^{commit}")) {
			on_line("2222222222222222222222222222222222222222\n");
			return exited(0);
		}
		if (unborn && has_arg(args, "HEAD^{commit}")) return exited(1);
		return inner(git, args, opts, on_line, stop);
	};
	make_service(std::move(options));
	const auto merged = [this] {
		return std::any_of(calls.begin(), calls.end(), [](const std::vector<std::string>& call) { return has_arg(call, "merge"); });
	};

	const json::value refused = finish(service->start_pull({}, {}, cmd::origin_kind::agent));
	BOOST_TEST(is_error(refused, "denied"), json::serialize(refused));
	BOOST_TEST(!merged());
	const auto diff = std::find_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) { return has_arg(call, "diff-tree"); });
	BOOST_REQUIRE(diff != calls.end());
	BOOST_TEST(has_arg(*diff, "2222222222222222222222222222222222222222"));
	BOOST_TEST(diff->back() == ".snk/agent_policy.json");

	changes_policy = false;
	BOOST_TEST(!finish(service->start_pull({}, {}, cmd::origin_kind::agent)).as_object().contains("error"));
	BOOST_TEST(merged());

	// No HEAD yet: cat-file -e finds the file in the target (the fake answers 0).
	calls.clear();
	unborn = true;
	BOOST_TEST(is_error(finish(service->start_pull({}, {}, cmd::origin_kind::agent)), "denied"));
	BOOST_TEST(!merged());
	BOOST_TEST(std::any_of(calls.begin(), calls.end(), [](const std::vector<std::string>& call) {
		return has_arg(call, "cat-file") && has_arg(call, "2222222222222222222222222222222222222222:.snk/agent_policy.json");
	}));
}

// Security review of #481, end to end: a personal policy that tries to switch a flag on is broken, effective_vcs is
// the strictest, and a vcs_service fed from it refuses an agent's push and fetch.
BOOST_AUTO_TEST_CASE(APersonalLayerThatAllowsAFlag_ClosesEveryAgentNetworkCall)
{
	bool network = false;
	command = [&network](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>&, std::stop_token) {
		if (has_arg(args, "push") || has_arg(args, "fetch")) network = true;
		return exited(0);
	};
	write(base / "project.json", R"({"vcs":{"push_branches":["*"],"remotes":["origin"]}})");
	write(base / "personal.json", R"({"vcs":{"allow_hooks":true}})");
	edt::agent_policy policy{ base / "project.json", base / "personal.json" };
	const edt::vcs_policy effective = policy.effective_vcs();
	BOOST_TEST(!policy.personal_error().empty());
	BOOST_TEST(effective.push_branches.empty());
	BOOST_TEST(effective.remotes.empty());
	BOOST_TEST(!effective.permissions.allow_hooks);

	make_service(fake_options());
	service->set_policy_source([&policy] { return policy.effective_vcs(); });
	BOOST_TEST(is_error(finish(service->start_push("origin", "agent/x", false, cmd::origin_kind::agent)), "denied"));
	BOOST_TEST(is_error(finish(service->start_fetch("origin", cmd::origin_kind::agent)), "denied"));
	BOOST_TEST(!network);
}

// A guarded pull that names a remote but no branch pulls the current branch's upstream only when the branch tracks
// that remote; otherwise the caller must name the branch.
BOOST_AUTO_TEST_CASE(AGuardedPull_OfANamedRemoteWithoutABranch_NeedsTheBranchToTrackIt)
{
	bool tracks = true;
	command = [&tracks](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "symbolic-ref")) on_line("main\n");
		if (has_arg(args, "branch.main.remote")) {
			if (!tracks) return exited(1);
			on_line("origin\n");
		}
		if (has_arg(args, "branch.main.merge")) on_line("refs/heads/main\n");
		return exited(0);
	};
	write(base / "FETCH_HEAD", "1111111111111111111111111111111111111111\t\tbranch 'main' of /elsewhere\n");
	make_service(fake_options());
	service->set_policy_source([] {
		edt::vcs_policy policy;
		policy.remotes = { "origin", "upstream" };
		return policy;
	});
	const auto fetches = [this] {
		return std::count_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) { return has_arg(call, "fetch"); });
	};
	const auto agent = cmd::origin_kind::agent;

	BOOST_TEST(is_error(finish(service->start_pull("upstream", {}, agent)), cmd::codes::invalid_args));
	BOOST_TEST(fetches() == 0);

	finish(service->start_pull("origin", {}, agent));
	BOOST_REQUIRE(fetches() == 1);
	const auto fetch = *std::find_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) { return has_arg(call, "fetch"); });
	BOOST_REQUIRE(fetch.size() >= 2u);
	BOOST_TEST(fetch[fetch.size() - 2] == "origin");
	BOOST_TEST(fetch.back() == "refs/heads/main");

	tracks = false;
	BOOST_TEST(is_error(finish(service->start_pull("origin", {}, agent)), cmd::codes::invalid_args));
	BOOST_TEST(fetches() == 1);
}

// A guarded checkout of a branch only a remote has checks out the very OID it checked, then sets the upstream; a
// failing upstream step is reported but the branch stays.
BOOST_AUTO_TEST_CASE(AGuardedCheckoutOfARemoteOnlyBranch_UsesTheCheckedOid)
{
	bool upstream_fails = false;
	command = [&upstream_fails](const std::vector<std::string>& args, const platform::process_options&,
		const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (has_arg(args, "for-each-ref")) {
			on_line("refs/remotes/origin/feature\n");
			return exited(0);
		}
		if (has_arg(args, "--set-upstream-to=refs/remotes/origin/feature") && upstream_fails) {
			platform::process_result result = exited(128);
			return result;
		}
		return exited(0);
	};
	auto options = fake_options();
	options.run = [inner = options.run](const fs::path& git, const std::vector<std::string>& args,
		const platform::process_options& opts, const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		if (has_arg(args, "refs/heads/feature^{commit}") || has_arg(args, "feature^{commit}")) return exited(1);
		if (has_arg(args, "refs/remotes/origin/feature^{commit}")) {
			on_line("3333333333333333333333333333333333333333\n");
			return exited(0);
		}
		return inner(git, args, opts, on_line, stop);
	};
	make_service(std::move(options));

	const json::value result = finish(service->start_checkout("feature", cmd::origin_kind::agent));
	BOOST_TEST(!result.as_object().contains("error"), json::serialize(result));
	const auto checkout = std::find_if(calls.begin(), calls.end(), [](const std::vector<std::string>& call) {
		return has_arg(call, "checkout") && has_arg(call, "-b");
	});
	BOOST_REQUIRE(checkout != calls.end());
	BOOST_REQUIRE(checkout->size() >= 4u);
	BOOST_TEST((*checkout)[checkout->size() - 4] == "-b");
	BOOST_TEST((*checkout)[checkout->size() - 3] == "feature");
	BOOST_TEST((*checkout)[checkout->size() - 2] == "3333333333333333333333333333333333333333");
	BOOST_TEST(checkout->back() == "--");
	BOOST_TEST(!has_arg(*checkout, "--track"));
	BOOST_TEST(!has_arg(*checkout, "refs/remotes/origin/feature"));
	const auto& upstream = calls.back();
	BOOST_TEST(has_arg(upstream, "branch"));
	BOOST_TEST(has_arg(upstream, "--set-upstream-to=refs/remotes/origin/feature"));
	BOOST_TEST(upstream.back() == "feature");
	// The guarded overrides apply to the upstream step too.
	BOOST_TEST(has_arg(upstream, "protocol.allow=never"));

	upstream_fails = true;
	const json::value failed = finish(service->start_checkout("feature", cmd::origin_kind::agent));
	BOOST_TEST(failed.as_object().contains("error"), json::serialize(failed));
	BOOST_TEST(has_arg(calls.back(), "--set-upstream-to=refs/remotes/origin/feature"));
}

BOOST_AUTO_TEST_SUITE_END()
