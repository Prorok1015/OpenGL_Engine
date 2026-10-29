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
#include <charconv>
#include <format>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
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
	constexpr std::size_t k_unlimited = std::numeric_limits<std::size_t>::max();

	// What a job needs to run git: copied into it, so a job never touches the service.
	struct git_context
	{
		fs::path git;
		std::vector<std::string> environment;
		fs::path cwd;
		edt::process_runner run;
	};

	struct git_output
	{
		platform::process_result process;
		std::string out;
		std::string err;
		// stdout reached the cap: git was stopped, the rest is missing.
		bool truncated = false;
	};

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
		std::vector<std::string> full{ "-c", "core.quotepath=off", "--no-optional-locks", "--literal-pathspecs" };
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

	json::value error_json(std::string_view code, std::string_view message, int exit_code)
	{
		const std::string text = edt::vcs::valid_utf8(edt::vcs::utf8_prefix(trim(message), k_message_cap));
		return json::object{ { "error", json::object{
			{ "code", json::string(code) }, { "message", json::string(text) }, { "exit_code", exit_code } } } };
	}

	// The job's answer when git did not do its job; nullopt when it did. A stop of the job ends it `cancelled`.
	std::optional<json::value> failure_of(const git_output& out, const ds::task_context& task)
	{
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		if (!out.process.error.empty()) return error_json("vcs_unavailable", out.process.error, -1);
		if (out.process.exit_code != 0 && !out.truncated) {
			return error_json(edt::vcs::classify_error(out.err), out.err, out.process.exit_code);
		}
		return std::nullopt;
	}

	std::optional<std::int64_t> skip_of(std::string_view cursor)
	{
		if (cursor.empty()) return 0;
		std::int64_t value = 0;
		const auto [end, error] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), value);
		if (error != std::errc{} || end != cursor.data() + cursor.size() || value < 0) return std::nullopt;
		return value;
	}
}

template<class F>
edt::vcs_service::started edt::vcs_service::run_job(std::string label, F work)
{
	if (!info().available) return unavailable();
	git_context context{ m_state.git, m_environment, *m_state.root, m_options.run };
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
	const git_output version = run_git({ git, m_environment, {}, m_options.run }, { "--version" }, {});
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
	const git_output top = run_git({ m_state.git, m_environment, *project, m_options.run }, { "rev-parse", "--show-toplevel" }, {});
	if (!top.process.error.empty()) {
		m_state.code = "vcs_unavailable";
		m_state.reason = std::format("git could not run in '{}': {}", utf8_of(*project), top.process.error);
		egLOG_WARN("editor/vcs", "{}", m_state.reason);
		return;
	}
	if (top.process.exit_code != 0) {
		const std::string_view kind = vcs::classify_error(top.err);
		m_state.code = kind == "not_a_repository" ? std::string(kind) : std::string("vcs_unavailable");
		m_state.reason = kind == "not_a_repository" ? std::string("not a repository")
			: vcs::valid_utf8(vcs::utf8_prefix(trim(top.err), k_message_cap));
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

edt::vcs_service::started edt::vcs_service::unavailable() const
{
	return { 0, m_state.code.empty() ? std::string("vcs_unavailable") : m_state.code, m_state.reason };
}

edt::vcs_service::started edt::vcs_service::start_status()
{
	return run_job("git status", [](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_git(git, { "status", "--porcelain=v2", "--branch", "-z" }, task.stop);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		return vcs::to_json(vcs::parse_status(out.out));
	});
}

edt::vcs_service::started edt::vcs_service::start_diff(const std::string& path, bool staged)
{
	std::vector<std::string> args{ "diff", "--no-color", "--no-ext-diff" };
	if (staged) args.emplace_back("--cached");
	if (!path.empty()) {
		args.emplace_back("--");
		args.push_back(path);
	}
	return run_job("git diff", [args = std::move(args)](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_git(git, args, task.stop, k_diff_cap);
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
	if (!ref.empty()) args.push_back(ref);
	if (!path.empty()) {
		args.emplace_back("--");
		args.push_back(path);
	}
	return run_job("git log", [args = std::move(args), count, first = *skip](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_git(git, args, task.stop);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		// A branch without commits has no log, which is not an error for the reader.
		if (out.process.error.empty() && out.process.exit_code != 0 && std::string_view(vcs::classify_error(out.err)) == "no_commits") {
			return json::object{ { "commits", json::array{} } };
		}
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		auto commits = vcs::parse_log(out.out);
		const bool more = commits.size() > static_cast<std::size_t>(count);
		if (more) commits.resize(static_cast<std::size_t>(count));
		json::array entries;
		entries.reserve(commits.size());
		for (const auto& commit : commits) entries.push_back(vcs::to_json(commit));
		json::object result{ { "commits", std::move(entries) } };
		if (more) result["next_cursor"] = json::string(std::to_string(first + count));
		return result;
	});
}

edt::vcs_service::started edt::vcs_service::start_branches()
{
	return run_job("git branches", [](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_git(git, { "for-each-ref", std::string(vcs::k_ref_format), "refs/heads", "refs/remotes" }, task.stop);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		json::array branches;
		json::object result;
		for (const auto& ref : vcs::parse_refs(out.out)) {
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
	status.summary = "Start reading git status (branch, upstream, ahead/behind, changed files); the result arrives in job.get";
	status.result_schema = json::parse(JOB_RESULT_SCHEMA);
	status.invoke = [&service](const cmd::call&) { return job_answer(service.start_status()); };
	keep(out, commands.add(std::move(status)));

	cmd::command_desc diff;
	diff.id = "vcs.diff";
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
	branches.summary = "Start listing the local and remote-tracking branches; the result arrives in job.get";
	branches.result_schema = json::parse(JOB_RESULT_SCHEMA);
	branches.invoke = [&service](const cmd::call&) { return job_answer(service.start_branches()); };
	keep(out, commands.add(std::move(branches)));
	return out;
}
