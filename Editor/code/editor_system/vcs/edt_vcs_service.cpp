#include "edt_vcs_service.h"
#include "edt_path_text.h"
#include "edt_vcs_parse.h"
#include <snk/command/cmd_json_schema.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <format>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

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

namespace fs = std::filesystem;
namespace json = boost::json;

namespace {
	constexpr std::size_t k_stderr_cap = 64 * 1024;
	constexpr std::size_t k_message_cap = 2000;
	constexpr std::size_t k_tail_lines = 20;
	constexpr std::size_t k_tail_bytes = 4 * 1024;
	constexpr std::size_t k_unlimited = std::numeric_limits<std::size_t>::max();
	// status, log, branches and the filter list: git is stopped beyond this and the answer says `truncated`.
	constexpr std::size_t k_read_cap = 4 * 1024 * 1024;
	// A cursor past this many commits is clamped: no history is that long, and skip + count stays far from overflow.
	constexpr std::int64_t k_max_skip = 1'000'000'000;

	// What a job needs to run git: copied into it, so a job never touches the service.
	struct git_context
	{
		fs::path git;
		std::vector<std::string> environment;
		fs::path cwd;
		edt::process_runner run;
		// Per call, after safe_base_args: the -c pairs that switch off the repository's filter drivers.
		std::vector<std::string> config_args;
	};

	struct git_output
	{
		platform::process_result process;
		std::string out;
		std::string err;
		// stdout reached the cap: git was stopped, the rest is missing.
		bool truncated = false;
		// Git was not run: the error code to report (with `err` as the message).
		std::string refused_code;
	};

	// A repository's own config (a project unzipped with its .git) must never run a program on a read: every key that
	// names a program git could start during status, diff, log, for-each-ref or rev-parse is overridden here, filter
	// drivers per call (switch_off_filters). Writes and the network come with #455.
	std::vector<std::string> safe_base_args()
	{
#ifdef _WIN32
		constexpr const char* no_hooks = "core.hooksPath=NUL";
#else
		constexpr const char* no_hooks = "core.hooksPath=/dev/null";
#endif
		return { "--no-pager",
			"-c", "core.quotepath=off",
			"-c", "core.fsmonitor=false",
			"-c", no_hooks,
			"-c", "diff.external=",
			"-c", "log.showSignature=false",
			"-c", "status.submoduleSummary=false",
			"-c", "diff.ignoreSubmodules=all",
			"--no-optional-locks", "--literal-pathspecs" };
	}

	// What the environment of git must not carry from the editor's: they would point git at another repository or
	// index, or add config (and with it programs) behind the -c overrides.
	bool is_unsafe_variable(std::string_view entry)
	{
		const std::size_t equals = entry.find('=', 1);
		if (equals == std::string_view::npos) return false;
		std::string name(entry.substr(0, equals));
		if (edt::k_native_env == edt::env_flavor::windows) {
			for (char& c : name) {
				if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
			}
		}
		for (const std::string_view unsafe : { "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
				"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE", "GIT_CONFIG_PARAMETERS",
				"GIT_CONFIG_COUNT", "GIT_EXTERNAL_DIFF" }) {
			if (name == unsafe) return true;
		}
		if (name.starts_with("GIT_CONFIG_KEY_") || name.starts_with("GIT_CONFIG_VALUE_")) return true;
		return name == "GIT_EXEC_PATH" && equals + 1 < entry.size();
	}

	fs::path path_of(std::string_view utf8)
	{
		return fs::path(std::u8string(utf8.begin(), utf8.end()));
	}

	std::string_view trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
		return text;
	}

	std::vector<std::string> own_environment()
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
		for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) out.emplace_back(*entry);
#endif
		return out;
	}

	git_output run_git(const git_context& git, const std::vector<std::string>& args, std::stop_token stop,
		std::size_t cap = k_unlimited)
	{
		git_output out;
		std::stop_source own;
		const std::stop_callback forward{ stop, [&own] { own.request_stop(); } };
		std::vector<std::string> full = safe_base_args();
		full.insert(full.end(), git.config_args.begin(), git.config_args.end());
		full.insert(full.end(), args.begin(), args.end());
		platform::process_options options;
		options.working_dir = git.cwd;
		options.environment = git.environment;
		// Bytes as git wrote them: -z output holds NULs, a diff its own line breaks.
		options.raw_output = true;
		options.on_stderr_line = [&out](std::string_view line) {
			if (out.err.size() >= k_stderr_cap) return;
			out.err.append(line);
			out.err.push_back('\n');
		};
		out.process = git.run(git.git, full, options, [&out, &own, cap](std::string_view chunk) {
			if (out.truncated) return;
			if (chunk.size() > cap - out.out.size()) {
				out.out.append(chunk.substr(0, cap - out.out.size()));
				out.truncated = true;
				own.request_stop();
				return;
			}
			out.out.append(chunk);
		}, own.get_token());
		return out;
	}

	// Empties clean, smudge and process of every filter driver the repository's config (or any other) names, plus
	// lfs, for the next call; config is read again per job, so an edit between two reads is seen. Nullopt when done;
	// otherwise what to report instead of running git.
	std::optional<git_output> switch_off_filters(git_context& git, std::stop_token stop)
	{
		git_output listed = run_git(git, { "config", "-z", "--get-regexp", "^filter\\." }, stop, k_read_cap);
		// Exit code 1: no such key.
		if (!listed.process.error.empty() || listed.truncated
			|| (listed.process.exit_code != 0 && listed.process.exit_code != 1)) {
			if (listed.truncated) {
				listed.refused_code = "unsafe_repository";
				listed.err = "fatal: the repository's config names too many filter drivers";
			}
			return listed;
		}
		std::vector<std::string> drivers{ "lfs" };
		std::size_t start = 0;
		while (start < listed.out.size()) {
			const std::size_t end = std::min(listed.out.find('\0', start), listed.out.size());
			const std::string_view record = std::string_view(listed.out).substr(start, end - start);
			start = end + 1;
			const std::string_view name = record.substr(0, record.find('\n'));
			constexpr std::string_view prefix = "filter.";
			const std::size_t last_dot = name.rfind('.');
			if (!name.starts_with(prefix) || last_dot == std::string_view::npos || last_dot <= prefix.size()) continue;
			const std::string driver(name.substr(prefix.size(), last_dot - prefix.size()));
			// "-c" splits at the first '=': such a name could not be overridden.
			const bool expressible = std::none_of(driver.begin(), driver.end(),
				[](char c) { return c == '=' || static_cast<unsigned char>(c) < 0x20; });
			if (!expressible) {
				listed.refused_code = "unsafe_repository";
				listed.err = std::format("fatal: the filter driver '{}' cannot be switched off", edt::vcs::valid_utf8(driver));
				return listed;
			}
			if (std::find(drivers.begin(), drivers.end(), driver) == drivers.end()) drivers.push_back(driver);
		}
		for (const auto& driver : drivers) {
			for (const char* key : { "clean=", "smudge=", "process=", "required=false" }) {
				git.config_args.emplace_back("-c");
				git.config_args.push_back(std::format("filter.{}.{}", driver, key));
			}
		}
		return std::nullopt;
	}

	// Every read: filters off first, then git.
	git_output run_read(git_context git, const std::vector<std::string>& args, std::stop_token stop,
		std::size_t cap = k_read_cap)
	{
		if (auto refused = switch_off_filters(git, stop)) return std::move(*refused);
		return run_git(git, args, stop, cap);
	}

	// A capped output ends inside a record: only the records up to the last `separator` are whole.
	std::string_view whole_records(const git_output& out, char separator)
	{
		if (!out.truncated) return out.out;
		const std::size_t last = out.out.rfind(separator);
		return last == std::string::npos ? std::string_view{} : std::string_view(out.out).substr(0, last + 1);
	}

	std::vector<std::string_view> lines_of(std::string_view text)
	{
		std::vector<std::string_view> out;
		while (!text.empty()) {
			const std::size_t end = text.find('\n');
			const std::string_view line = trim(text.substr(0, end));
			if (!line.empty()) out.push_back(line);
			if (end == std::string_view::npos) break;
			text.remove_prefix(end + 1);
		}
		return out;
	}

	// The last k_tail_lines non-empty lines of stderr, at most k_tail_bytes (the end is kept).
	std::string stderr_tail(std::string_view err)
	{
		const auto lines = lines_of(err);
		std::string tail;
		for (std::size_t i = lines.size() > k_tail_lines ? lines.size() - k_tail_lines : 0; i < lines.size(); ++i) {
			if (!tail.empty()) tail.push_back('\n');
			tail.append(lines[i]);
		}
		if (tail.size() > k_tail_bytes) {
			std::size_t start = tail.size() - k_tail_bytes;
			while (start < tail.size() && (static_cast<unsigned char>(tail[start]) & 0xC0) == 0x80) ++start;
			tail.erase(0, start);
		}
		return edt::vcs::valid_utf8(tail);
	}

	// The line that says what went wrong: git's last "fatal:" or "error:" line, else its last line.
	std::string summary_of(std::string_view err, int exit_code)
	{
		const auto lines = lines_of(err);
		for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
			if (it->starts_with("fatal:") || it->starts_with("error:")) return std::string(*it);
		}
		if (!lines.empty()) return std::string(lines.back());
		return std::format("git exited with code {}", exit_code);
	}

	json::value error_json(std::string_view code, std::string_view message, std::string_view err, int exit_code)
	{
		const std::string text = edt::vcs::valid_utf8(edt::vcs::utf8_prefix(trim(message), k_message_cap));
		return json::object{ { "error", json::object{
			{ "code", json::string(code) },
			{ "message", json::string(text) },
			{ "stderr_tail", json::string(stderr_tail(err)) },
			{ "exit_code", exit_code } } } };
	}

	// The job's answer when git did not do its job; nullopt when it did. A stop of the job ends it `cancelled`.
	std::optional<json::value> failure_of(const git_output& out, const ds::task_context& task)
	{
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		if (!out.refused_code.empty()) return error_json(out.refused_code, out.err, out.err, -1);
		if (!out.process.error.empty()) return error_json("vcs_unavailable", out.process.error, out.err, -1);
		if (out.process.exit_code != 0 && !out.truncated) {
			return error_json(edt::vcs::classify_error(out.err), summary_of(out.err, out.process.exit_code), out.err,
				out.process.exit_code);
		}
		return std::nullopt;
	}

	// A probe on the main thread: git is stopped after `timeout`. `timed_out` tells whether that happened.
	git_output run_probe(const git_context& git, const std::vector<std::string>& args, std::chrono::milliseconds timeout,
		bool& timed_out)
	{
		std::stop_source stop;
		std::atomic<bool> fired = false;
		std::atomic<bool> returned = false;
		git_output out;
		{
			// Destroyed (stop requested, joined) as soon as git returns: no thread outlives the probe.
			std::jthread watchdog([&stop, &fired, &returned, timeout](std::stop_token own) {
				std::mutex mutex;
				std::condition_variable_any wake;
				std::unique_lock lock(mutex);
				static_cast<void>(wake.wait_for(lock, own, timeout, [] { return false; }));
				if (!own.stop_requested() && !returned) {
					fired = true;
					stop.request_stop();
				}
			});
			out = run_git(git, args, stop.get_token());
			returned = true;
		}
		// A git that finished as the deadline hit (not stopped by it) answered in time.
		timed_out = fired && out.process.cancelled;
		return out;
	}

	std::optional<std::int64_t> skip_of(std::string_view cursor)
	{
		if (cursor.empty()) return 0;
		std::int64_t value = 0;
		const auto [end, error] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), value);
		if (end != cursor.data() + cursor.size()) return std::nullopt;
		if (error == std::errc::result_out_of_range && cursor.front() != '-') return k_max_skip;
		if (error != std::errc{} || value < 0) return std::nullopt;
		return std::min(value, k_max_skip);
	}
}

template<class F>
edt::vcs_service::started edt::vcs_service::run_job(std::string label, F work)
{
	// Only "no git" and "no project" refuse at once; whatever git itself reports (not a repository, ...) is the job's
	// result, so every command answers the same way.
	const state& now = info();
	if (!m_git_found || !m_probed_project || now.code == "vcs_unavailable" || now.code == "no_project") return unavailable();
	git_context context{ m_state.git, m_environment, now.root ? *now.root : *m_probed_project, m_options.run, {} };
	const auto id = m_jobs.start_with_result(ds::lane::io, ds::task_desc{ std::move(label), "vcs" }, m_life,
		[context = std::move(context), work = std::move(work)](ds::task_context& task) -> json::value {
			return work(context, task);
		});
	return { id, {}, {} };
}

edt::vcs_service::vcs_service(job_service& jobs, vcs_options options)
	: m_jobs(jobs)
	, m_options(std::move(options))
{
	m_environment = m_options.environment ? *m_options.environment : own_environment();
	std::erase_if(m_environment, [](const std::string& entry) { return is_unsafe_variable(entry); });
	// Plain English messages for classify_error, and nothing that waits for a person.
	set_variable(m_environment, "LC_ALL", "C");
	set_variable(m_environment, "GIT_TERMINAL_PROMPT", "0");
	set_variable(m_environment, "GCM_INTERACTIVE", "never");
	if (!variable_of(m_environment, "GIT_SSH_COMMAND")) set_variable(m_environment, "GIT_SSH_COMMAND", "ssh -oBatchMode=yes");

	fs::path git = m_options.git;
	if (git.empty()) {
		if (auto found = find_on_path(m_environment, k_native_env == env_flavor::windows ? "git.exe" : "git")) git = std::move(*found);
	}
	if (git.empty()) {
		m_state.code = "vcs_unavailable";
		m_state.reason = "git not found";
		egLOG_INFO("editor/vcs", "git not found on PATH: version control is off (set vcs/git)");
		return;
	}
	bool timed_out = false;
	const git_output version = run_probe({ git, m_environment, {}, m_options.run, {} }, { "--version" }, m_options.probe_timeout,
		timed_out);
	if (timed_out) {
		set_probe_timeout();
		return;
	}
	if (!version.process.error.empty() || version.process.exit_code != 0) {
		m_state.code = "vcs_unavailable";
		m_state.reason = std::format("git not found ('{}': {})", utf8_of(git), version.process.error.empty()
			? std::format("exit code {}", version.process.exit_code) : version.process.error);
		egLOG_WARN("editor/vcs", "{}: version control is off", m_state.reason);
		return;
	}
	m_git_found = true;
	m_state.git = git;
	m_state.git_version = vcs::parse_version(version.out);
	probe_repository(m_options.project_root ? m_options.project_root() : std::nullopt);
}

edt::vcs_service::~vcs_service() = default;

void edt::vcs_service::probe_repository(const std::optional<fs::path>& project)
{
	m_probed = true;
	m_probed_project = project;
	if (!m_git_found) return;
	m_state.available = false;
	m_state.root.reset();
	if (!project) {
		m_state.code = "no_project";
		m_state.reason = "no project is open";
		return;
	}
	bool timed_out = false;
	const git_output top = run_probe({ m_state.git, m_environment, *project, m_options.run, {} }, { "rev-parse", "--show-toplevel" },
		m_options.probe_timeout, timed_out);
	if (timed_out) {
		set_probe_timeout();
		return;
	}
	if (!top.process.error.empty()) {
		m_state.code = "vcs_unavailable";
		m_state.reason = std::format("git could not run in '{}': {}", utf8_of(*project), top.process.error);
		egLOG_WARN("editor/vcs", "{}", m_state.reason);
		return;
	}
	if (top.process.exit_code != 0) {
		const std::string_view kind = vcs::classify_error(top.err);
		m_state.code = kind;
		m_state.reason = kind == "not_a_repository" ? std::string("not a repository")
			: vcs::valid_utf8(vcs::utf8_prefix(summary_of(top.err, top.process.exit_code), k_message_cap));
		egLOG_INFO("editor/vcs", "'{}': {}", utf8_of(*project), m_state.reason);
		return;
	}
	m_state.root = path_of(trim(top.out));
	m_state.available = true;
	m_state.code.clear();
	m_state.reason.clear();
	egLOG_INFO("editor/vcs", "git {} on the repository '{}'", m_state.git_version, utf8_of(*m_state.root));
}

const edt::vcs_service::state& edt::vcs_service::info()
{
	auto project = m_options.project_root ? m_options.project_root() : std::nullopt;
	if (!m_probed || project != m_probed_project) probe_repository(project);
	return m_state;
}

void edt::vcs_service::set_probe_timeout()
{
	m_state.available = false;
	m_state.code = "vcs_unavailable";
	const auto ms = m_options.probe_timeout.count();
	m_state.reason = ms % 1000 == 0 ? std::format("git did not answer within {} s", ms / 1000)
		: std::format("git did not answer within {} ms", ms);
	egLOG_WARN("editor/vcs", "{}: version control is off", m_state.reason);
}

edt::vcs_service::started edt::vcs_service::unavailable() const
{
	return { 0, m_state.code.empty() ? std::string("vcs_unavailable") : m_state.code, m_state.reason };
}

edt::vcs_service::started edt::vcs_service::start_status()
{
	return run_job("git status", [](const git_context& git, ds::task_context& task) -> json::value {
		// Submodules are not entered: a submodule's config is as untrusted as the repository's.
		const git_output out = run_read(git, { "status", "--porcelain=v2", "--branch", "-z", "--ignore-submodules=all" },
			task.stop);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		json::value result = vcs::to_json(vcs::parse_status(whole_records(out, '\0')));
		result.as_object()["truncated"] = out.truncated;
		return result;
	});
}

edt::vcs_service::started edt::vcs_service::start_diff(const std::string& path, bool staged)
{
	std::vector<std::string> args{ "diff", "--no-color", "--no-ext-diff", "--no-textconv", "--ignore-submodules=all" };
	if (staged) args.emplace_back("--cached");
	if (!path.empty()) {
		args.emplace_back("--");
		args.push_back(path);
	}
	return run_job("git diff", [args = std::move(args)](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_read(git, args, task.stop, k_diff_cap);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		std::string text = vcs::valid_utf8(vcs::utf8_prefix(out.out, k_diff_cap));
		bool truncated = out.truncated;
		// Replacement characters may have made it longer than the cap.
		if (text.size() > k_diff_cap) {
			text = std::string(vcs::utf8_prefix(text, k_diff_cap));
			truncated = true;
		}
		return json::object{ { "text", json::string(text) }, { "truncated", truncated } };
	});
}

edt::vcs_service::started edt::vcs_service::start_log(const std::string& ref, const std::string& path, int limit,
	const std::string& cursor)
{
	if (!ref.empty() && ref.front() == '-') return { 0, cmd::codes::invalid_args, "ref must not start with '-'" };
	const auto skip = skip_of(cursor);
	if (!skip) return { 0, cmd::codes::invalid_args, "cursor is not one that vcs.log returned" };
	const int count = std::clamp(limit, 1, k_max_log_limit);
	// One more than asked: whether it comes back tells if there is a next page.
	std::vector<std::string> args{ "log", "-z", std::string(vcs::k_log_format), std::format("--skip={}", *skip),
		std::format("--max-count={}", count + 1) };
	// Whatever the ref and the path hold, git reads them as a revision and a path, never as options.
	args.emplace_back("--end-of-options");
	if (!ref.empty()) args.push_back(ref);
	args.emplace_back("--");
	if (!path.empty()) args.push_back(path);
	return run_job("git log", [args = std::move(args), count, first = *skip](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_read(git, args, task.stop);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		// A branch without commits has no log, which is not an error for the reader.
		if (out.process.error.empty() && out.process.exit_code != 0 && std::string_view(vcs::classify_error(out.err)) == "no_commits") {
			return json::object{ { "commits", json::array{} } };
		}
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		auto commits = vcs::parse_log(whole_records(out, '\0'));
		const bool more = out.truncated || commits.size() > static_cast<std::size_t>(count);
		if (commits.size() > static_cast<std::size_t>(count)) commits.resize(static_cast<std::size_t>(count));
		json::array entries;
		entries.reserve(commits.size());
		for (const auto& commit : commits) entries.push_back(vcs::to_json(commit));
		json::object result{ { "commits", std::move(entries) }, { "truncated", out.truncated } };
		if (more) result["next_cursor"] = json::string(std::to_string(first + static_cast<std::int64_t>(commits.size())));
		return result;
	});
}

edt::vcs_service::started edt::vcs_service::start_branches()
{
	return run_job("git branches", [](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_read(git, { "for-each-ref", std::string(vcs::k_ref_format), "refs/heads", "refs/remotes" },
			task.stop);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		json::array branches;
		json::object result{ { "truncated", out.truncated } };
		for (const auto& ref : vcs::parse_refs(whole_records(out, '\n'))) {
			if (ref.head && !ref.remote) result["current"] = json::string(vcs::valid_utf8(ref.name));
			branches.push_back(vcs::to_json(ref));
		}
		result["branches"] = std::move(branches);
		return result;
	});
}

namespace {
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

	void keep(std::vector<edt::registration>& out, edt::registration added)
	{
		if (added.active()) out.push_back(std::move(added));
	}
}

std::vector<edt::registration> edt::register_vcs_commands(cmd::command_registry& commands, vcs_service& service)
{
	std::vector<registration> out;

	cmd::command_desc info;
	info.id = "vcs.info";
	info.category = cmd::agent_category::read;
	info.summary = "Whether git is available for the open project: its repository root and the git version";
	info.result_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"available": { "type": "boolean" },
			"reason": { "type": "string" },
			"code": { "type": "string" },
			"root": { "type": "string" },
			"git_version": { "type": "string" }
		},
		"required": ["available"],
		"additionalProperties": false
	})");
	info.invoke = [&service](const cmd::call&) {
		const auto& state = service.info();
		json::object result{ { "available", state.available } };
		if (!state.reason.empty()) result["reason"] = json::string(state.reason);
		if (!state.code.empty()) result["code"] = json::string(state.code);
		if (state.root) result["root"] = json::string(vcs::valid_utf8(utf8_of(*state.root)));
		if (!state.git_version.empty()) result["git_version"] = json::string(state.git_version);
		return cmd::result::success(std::move(result));
	};
	keep(out, commands.add(std::move(info)));

	cmd::command_desc status;
	status.id = "vcs.status";
	status.category = cmd::agent_category::read;
	status.summary = "Start reading git status (branch, upstream, ahead/behind, changed files); the result arrives in job.get";
	status.result_schema = json::parse(JOB_RESULT_SCHEMA);
	status.invoke = [&service](const cmd::call&) { return job_answer(service.start_status()); };
	keep(out, commands.add(std::move(status)));

	cmd::command_desc diff;
	diff.id = "vcs.diff";
	diff.category = cmd::agent_category::read;
	diff.summary = "Start reading the working tree diff (or the staged one), of one path or all; text capped at 256 KiB; "
		"the result arrives in job.get";
	diff.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"path": { "type": "string", "minLength": 1, "description": "Relative to the repository root" },
			"staged": { "type": "boolean" }
		},
		"additionalProperties": false
	})");
	diff.result_schema = json::parse(JOB_RESULT_SCHEMA);
	diff.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		bool staged = false;
		if (const json::value* value = args.if_contains("staged")) staged = value->as_bool();
		return job_answer(service.start_diff(string_arg(args, "path"), staged));
	};
	keep(out, commands.add(std::move(diff)));

	cmd::command_desc log;
	log.id = "vcs.log";
	log.category = cmd::agent_category::read;
	log.summary = "Start reading the commit log of a ref (default HEAD), optionally of one path, one page at a time; "
		"the result arrives in job.get";
	log.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"ref": { "type": "string", "minLength": 1 },
			"path": { "type": "string", "minLength": 1, "description": "Relative to the repository root" },
			"limit": { "type": "integer", "minimum": 1, "maximum": 200 },
			"cursor": { "type": "string", "description": "next_cursor of the previous page" }
		},
		"additionalProperties": false
	})");
	log.result_schema = json::parse(JOB_RESULT_SCHEMA);
	log.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		int limit = vcs_service::k_default_log_limit;
		if (const json::value* value = args.if_contains("limit")) {
			limit = static_cast<int>(cmd::json_schema::integer_of(*value).value_or(vcs_service::k_default_log_limit));
		}
		return job_answer(service.start_log(string_arg(args, "ref"), string_arg(args, "path"), limit, string_arg(args, "cursor")));
	};
	keep(out, commands.add(std::move(log)));

	cmd::command_desc branches;
	branches.id = "vcs.branches";
	branches.category = cmd::agent_category::read;
	branches.summary = "Start listing the local and remote-tracking branches; the result arrives in job.get";
	branches.result_schema = json::parse(JOB_RESULT_SCHEMA);
	branches.invoke = [&service](const cmd::call&) { return job_answer(service.start_branches()); };
	keep(out, commands.add(std::move(branches)));
	return out;
}
