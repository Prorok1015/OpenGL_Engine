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
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
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
	// The file lists of the filter check (the whole index): past this the check fails closed.
	constexpr std::size_t k_list_cap = 64 * 1024 * 1024;
	// A cursor past this many commits is clamped: no history is that long, and skip + count stays far from overflow.
	constexpr std::int64_t k_max_skip = 1'000'000'000;

	// What a job needs to run git: copied into it, so a job never touches the service.
	struct git_context
	{
		fs::path git;
		std::vector<std::string> environment;
		fs::path cwd;
		edt::process_runner run;
		// Per call, after safe_base_args: the hooks and filter overrides of a read, the guard overrides of a write.
		std::vector<std::string> config_args;
		// A write not from a person (origin agent, test, game): the guarded path.
		bool guarded = false;
		// Guarded, without allow_any_repo_config: the repository's own config must pass the allowlist (config_refusal).
		bool check_repo_config = false;
		// Guarded, with allow_any_repo_config, on git before 2.45 (no GIT_NO_LAZY_FETCH): a partial clone still refuses.
		bool refuse_partial_clone = false;
		bool allow_hooks = false;
		// Guarded write that can run filter drivers, without allow_filters: refused when a configured driver applies
		// to a file it touches.
		bool refuse_filters = false;
		// Guarded network call: refused when the repository's config would pick a program it runs (network_refusal).
		bool guard_network = false;
		bool allow_repo_credential_helper = false;
		// git 2.40+: check-attr --source=<tree> reads the attributes of a checkout's target.
		bool attr_source = false;
		// Guarded network call: the remotes the agent policy allows (remote_refusal).
		std::vector<std::string> allowed_remotes;
		// Guarded: the project's agent policy file (absolute; empty: none), which a checkout or pull must not change.
		fs::path policy_file;
		// git 2.30+: rev-parse takes --end-of-options.
		bool rev_parse_end_of_options = false;
	};

	git_context make_context(fs::path git, std::vector<std::string> environment, fs::path cwd, edt::process_runner run)
	{
		git_context context;
		context.git = std::move(git);
		context.environment = std::move(environment);
		context.cwd = std::move(cwd);
		context.run = std::move(run);
		return context;
	}

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
	// names a program git could start during status, diff, log, for-each-ref or rev-parse is overridden here, hooks
	// and filter drivers per read (run_read). Every call, writes included, gets these.
	std::vector<std::string> safe_base_args()
	{
		return { "--no-pager",
			"-c", "core.quotepath=off",
			"-c", "core.fsmonitor=false",
			"-c", "diff.external=",
			"-c", "log.showSignature=false",
			"-c", "status.submoduleSummary=false",
			"-c", "diff.ignoreSubmodules=all",
			"--no-optional-locks", "--literal-pathspecs" };
	}

	// The name of a NAME=value entry, upper case on Windows (which ignores its case); empty without '='.
	std::string variable_name(std::string_view entry)
	{
		const std::size_t equals = entry.find('=', 1);
		if (equals == std::string_view::npos) return {};
		std::string name(entry.substr(0, equals));
		if (edt::k_native_env == edt::env_flavor::windows) {
			for (char& c : name) {
				if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
			}
		}
		return name;
	}

	// A program that would be asked for a password: an agent's git never starts one.
	bool is_askpass_variable(std::string_view entry)
	{
		const std::string name = variable_name(entry);
		return name == "GIT_ASKPASS" || name == "SSH_ASKPASS";
	}

	// What the environment of git must not carry from the editor's: they would point git at another repository or
	// index, add config (and with it programs) behind the -c overrides, or allow transports (GIT_ALLOW_PROTOCOL wins
	// over protocol.allow).
	bool is_unsafe_variable(std::string_view entry)
	{
		const std::size_t equals = entry.find('=', 1);
		if (equals == std::string_view::npos) return false;
		const std::string name = variable_name(entry);
		for (const std::string_view unsafe : { "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
				"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE", "GIT_CONFIG_PARAMETERS",
				"GIT_CONFIG_COUNT", "GIT_EXTERNAL_DIFF", "GIT_ALLOW_PROTOCOL", "GIT_PROTOCOL_FROM_USER" }) {
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

	// The driver names of `git config -z --get-regexp ^filter\.`: "filter.<name>.<key>\n<value>\0" records, each name
	// once, in order.
	std::vector<std::string> drivers_of(std::string_view listing)
	{
		std::vector<std::string> drivers;
		std::size_t start = 0;
		while (start < listing.size()) {
			const std::size_t end = std::min(listing.find('\0', start), listing.size());
			const std::string_view record = listing.substr(start, end - start);
			start = end + 1;
			const std::string_view name = record.substr(0, record.find('\n'));
			constexpr std::string_view prefix = "filter.";
			const std::size_t last_dot = name.rfind('.');
			if (!name.starts_with(prefix) || last_dot == std::string_view::npos || last_dot <= prefix.size()) continue;
			std::string driver(name.substr(prefix.size(), last_dot - prefix.size()));
			if (std::find(drivers.begin(), drivers.end(), driver) == drivers.end()) drivers.push_back(std::move(driver));
		}
		return drivers;
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
		for (auto& driver : drivers_of(listed.out)) {
			// "-c" splits at the first '=': such a name could not be overridden.
			const bool expressible = std::none_of(driver.begin(), driver.end(),
				[](char c) { return c == '=' || static_cast<unsigned char>(c) < 0x20; });
			if (!expressible) {
				listed.refused_code = "unsafe_repository";
				listed.err = std::format("fatal: the filter driver '{}' cannot be switched off", edt::vcs::valid_utf8(driver));
				return listed;
			}
			if (std::find(drivers.begin(), drivers.end(), driver) == drivers.end()) drivers.push_back(std::move(driver));
		}
		for (const auto& driver : drivers) {
			for (const char* key : { "clean=", "smudge=", "process=", "required=false" }) {
				git.config_args.emplace_back("-c");
				git.config_args.push_back(std::format("filter.{}.{}", driver, key));
			}
		}
		return std::nullopt;
	}

	// Makes `git` a read's context: no hooks, filters off. Nullopt when done; otherwise what to report instead.
	std::optional<git_output> harden_read(git_context& git, std::stop_token stop)
	{
#ifdef _WIN32
		constexpr const char* no_hooks = "core.hooksPath=NUL";
#else
		constexpr const char* no_hooks = "core.hooksPath=/dev/null";
#endif
		git.config_args.insert(git.config_args.begin(), { "-c", no_hooks });
		return switch_off_filters(git, stop);
	}

	// Every read: no hooks, filters off first, then git.
	git_output run_read(git_context git, const std::vector<std::string>& args, std::stop_token stop,
		std::size_t cap = k_read_cap)
	{
		if (auto refused = harden_read(git, stop)) return std::move(*refused);
		return run_git(git, args, stop, cap);
	}

	// A write's git. Output is not capped: stopping a write half way is worse than its text.
	git_output run_write(const git_context& git, const std::vector<std::string>& args, std::stop_token stop)
	{
		return run_git(git, args, stop);
	}

	// "2.45.1", "2.39.2.windows.1": whether it is at least major.minor. A version that does not read so is not.
	bool version_at_least(std::string_view version, int major, int minor)
	{
		const char* const end = version.data() + version.size();
		int found_major = 0;
		const auto [after_major, major_error] = std::from_chars(version.data(), end, found_major);
		if (major_error != std::errc{} || after_major == end || *after_major != '.') return false;
		int found_minor = 0;
		if (std::from_chars(after_major + 1, end, found_minor).ec != std::errc{}) return false;
		return found_major > major || (found_major == major && found_minor >= minor);
	}

	// What to report instead of running git.
	git_output refused(std::string_view code, std::string message)
	{
		git_output out;
		out.refused_code = std::string(code);
		out.err = std::move(message);
		return out;
	}

	bool listing_failed(const git_output& out)
	{
		// Exit code 1 of `git config --get*`: no such key.
		return !out.process.error.empty() || (out.process.exit_code != 0 && out.process.exit_code != 1);
	}

	// NUL-terminated fields; the empty one after the last NUL is not a field.
	std::vector<std::string> split_nul(std::string_view text)
	{
		std::vector<std::string> fields;
		std::size_t start = 0;
		while (start < text.size()) {
			const std::size_t end = std::min(text.find('\0', start), text.size());
			fields.emplace_back(text.substr(start, end - start));
			start = end + 1;
		}
		return fields;
	}

	std::string names_of(const std::vector<std::string>& names)
	{
		std::string out;
		for (const auto& name : names) {
			if (out.size() > 200) {
				out += ", ...";
				break;
			}
			if (!out.empty()) out += ", ";
			out += edt::vcs::valid_utf8(name);
		}
		return out;
	}

	bool succeeded(const git_output& out)
	{
		return out.process.error.empty() && out.process.exit_code == 0 && !out.truncated;
	}

	// The files a write touches, for the filter check. Always the whole index: reset, commit, checkout and merge re-hash
	// any tracked file whose stat is stale (all of them in a project just unzipped) through its clean filter. Besides:
	// `paths` (stage, unstage) the untracked files under the paths and the paths, `target` (checkout, pull) what
	// differs between HEAD and the target.
	enum class touched_kind : std::uint8_t { index, paths, target };

	struct touched_files
	{
		touched_kind kind = touched_kind::index;
		std::vector<std::string> paths;
		// An OID or a full ref name, resolved once by the caller.
		std::string target;
	};

	struct touched_list
	{
		std::vector<std::string> files;
		// `target` only: the files that differ from HEAD.
		std::vector<std::string> changed;
	};

	// Relative to the repository root; nullopt when git could not tell.
	std::optional<touched_list> touched_paths(const git_context& git, const touched_files& touched, std::stop_token stop)
	{
		const git_output index = run_git(git, { "ls-files", "-z", "--cached" }, stop, k_list_cap);
		if (!succeeded(index)) return std::nullopt;
		touched_list list;
		list.files = split_nul(index.out);
		if (touched.kind == touched_kind::paths) {
			std::vector<std::string> args{ "ls-files", "-z", "--others", "--exclude-standard", "--" };
			args.insert(args.end(), touched.paths.begin(), touched.paths.end());
			const git_output others = run_git(git, args, stop, k_list_cap);
			if (!succeeded(others)) return std::nullopt;
			for (auto& file : split_nul(others.out)) list.files.push_back(std::move(file));
			list.files.insert(list.files.end(), touched.paths.begin(), touched.paths.end());
		}
		else if (touched.kind == touched_kind::target) {
			git_output diff = run_git(git, { "diff", "--name-only", "--no-renames", "-z", "HEAD", touched.target, "--" },
				stop, k_list_cap);
			// A branch without commits: every file of the target.
			if (diff.process.error.empty() && diff.process.exit_code != 0) {
				diff = run_git(git, { "ls-tree", "-r", "-z", "--name-only", "--full-tree", touched.target }, stop, k_list_cap);
			}
			if (!succeeded(diff)) return std::nullopt;
			list.changed = split_nul(diff.out);
			list.files.insert(list.files.end(), list.changed.begin(), list.changed.end());
		}
		return list;
	}

	// How a guarded checkout runs, decided once so that the filter check and the checkout see the same tree.
	enum class checkout_kind : std::uint8_t { branch, commit, remote_branch };

	struct checkout_plan
	{
		checkout_kind kind = checkout_kind::branch;
		// The OID (branch, commit) or refs/remotes/<remote>/<name> (remote_branch) the filter check diffs against.
		std::string target;
	};

	// <name> as a local branch (refs/heads/<name>) first, else as a commit (a tag, a hash, any other ref; as its OID:
	// `git diff HEAD x` would take tag x where checkout takes branch x), else the branch of exactly one remote.
	// Nullopt: none of them.
	std::optional<checkout_plan> plan_checkout(const git_context& git, const std::string& name, std::stop_token stop)
	{
		const git_output branch = run_git(git, { "rev-parse", "--verify", "--quiet", "refs/heads/" + name + "^{commit}" },
			stop, k_read_cap);
		if (succeeded(branch) && !trim(branch.out).empty()) {
			return checkout_plan{ checkout_kind::branch, std::string(trim(branch.out)) };
		}
		const git_output commit = run_git(git, { "rev-parse", "--verify", "--quiet", name + "^{commit}" }, stop, k_read_cap);
		if (succeeded(commit) && !trim(commit.out).empty()) {
			return checkout_plan{ checkout_kind::commit, std::string(trim(commit.out)) };
		}
		const git_output refs = run_git(git, { "for-each-ref", "--format=%(refname)", "refs/remotes" }, stop, k_read_cap);
		if (!succeeded(refs)) return std::nullopt;
		constexpr std::string_view prefix = "refs/remotes/";
		const std::string suffix = "/" + name;
		std::string found;
		int matches = 0;
		std::string_view rest = refs.out;
		while (!rest.empty()) {
			const std::size_t end = std::min(rest.find('\n'), rest.size());
			const std::string_view line = trim(rest.substr(0, end));
			rest.remove_prefix(std::min(end + 1, rest.size()));
			if (!line.starts_with(prefix) || !line.ends_with(suffix) || line.size() <= prefix.size() + suffix.size()) continue;
			const std::string_view remote = line.substr(prefix.size(), line.size() - prefix.size() - suffix.size());
			if (remote.find('/') != std::string_view::npos) continue;
			++matches;
			found = std::string(line);
		}
		if (matches != 1) return std::nullopt;
		return checkout_plan{ checkout_kind::remote_branch, found };
	}

	// The drivers of `configured` that `git check-attr <source> filter` names for `files`, added to `found`; false when
	// git could not tell. `source`: nothing (the work tree's .gitattributes first), "--cached" (the index's) or
	// "--source=<tree>" (a target's); .git/info/attributes and core.attributesFile always count.
	bool applied_drivers(const git_context& git, const std::vector<std::string>& files,
		const std::vector<std::string>& configured, const std::string& source, std::vector<std::string>& found,
		std::stop_token stop)
	{
		std::size_t next = 0;
		while (next < files.size()) {
			std::vector<std::string> args{ "check-attr", "-z" };
			if (!source.empty()) args.push_back(source);
			args.insert(args.end(), { "filter", "--" });
			std::size_t bytes = 0;
			while (next < files.size() && (bytes == 0 || bytes + files[next].size() < edt::vcs_service::k_max_paths_bytes)) {
				bytes += files[next].size() + 1;
				args.push_back(files[next++]);
			}
			const git_output out = run_git(git, args, stop, k_read_cap);
			if (!succeeded(out)) return false;
			// path, "filter", value: "unspecified", "unset", "set" or a driver name.
			const auto fields = split_nul(out.out);
			for (std::size_t i = 0; i + 2 < fields.size(); i += 3) {
				const std::string& value = fields[i + 2];
				if (std::find(configured.begin(), configured.end(), value) != configured.end()
					&& std::find(found.begin(), found.end(), value) == found.end()) {
					found.push_back(value);
				}
			}
		}
		return true;
	}

	bool is_attributes_file(std::string_view path)
	{
		return path == ".gitattributes" || path.ends_with("/.gitattributes");
	}

	// Without check-attr --source (git < 2.40): whether a tracked .gitattributes of the work tree differs from the
	// index's (`diff --quiet`, explicit paths: the calls run with --literal-pathspecs). Nullopt: git could not tell.
	std::optional<bool> attributes_differ_from_index(const git_context& git, const std::vector<std::string>& index_files,
		std::stop_token stop)
	{
		std::vector<std::string> attributes;
		std::copy_if(index_files.begin(), index_files.end(), std::back_inserter(attributes),
			[](const std::string& file) { return is_attributes_file(file); });
		// Comparing the work tree with the index re-hashes through clean filters: these run with every driver emptied.
		git_context quiet = git;
		if (!attributes.empty() && switch_off_filters(quiet, stop)) return std::nullopt;
		std::size_t next = 0;
		while (next < attributes.size()) {
			std::vector<std::string> args{ "diff", "--quiet", "--no-ext-diff", "--no-textconv", "--" };
			std::size_t bytes = 0;
			while (next < attributes.size() && (bytes == 0 || bytes + attributes[next].size() < edt::vcs_service::k_max_paths_bytes)) {
				bytes += attributes[next].size() + 1;
				args.push_back(attributes[next++]);
			}
			const git_output out = run_git(quiet, args, stop, k_read_cap);
			if (!out.process.error.empty() || out.truncated) return std::nullopt;
			if (out.process.exit_code == 1) return true;
			if (out.process.exit_code != 0) return std::nullopt;
		}
		return false;
	}

	// A guarded write without allow_filters: refused (filters_require_opt_in) when a configured clean, smudge or process
	// driver applies to a file it touches. Nullopt: git may run.
	std::optional<git_output> filter_refusal(const git_context& git, const touched_files& touched, std::stop_token stop)
	{
		if (!git.refuse_filters) return std::nullopt;
		git_output listed = run_git(git, { "config", "-z", "--get-regexp", "^filter\\..*\\.(clean|smudge|process)$" },
			stop, k_read_cap);
		if (listing_failed(listed)) return listed;
		const auto configured = drivers_of(listed.out);
		if (configured.empty() && !listed.truncated) return std::nullopt;
		const auto refuse = [](std::string why) {
			return refused("filters_require_opt_in",
				why + "; a git write not made by a person runs filter drivers only when filters are allowed for it");
		};
		if (listed.truncated) return refuse("the repository's git config sets too many filter drivers to check");
		const std::string names = names_of(configured);
		const auto list = touched_paths(git, touched, stop);
		if (!list) return refuse(std::format("filter drivers ({}) are configured and git could not tell which files this write touches", names));
		// The target's attributes may differ from the work tree's, which check-attr reads.
		for (const auto& file : list->changed) {
			if (is_attributes_file(file)) {
				return refuse(std::format("this write changes '{}' while filter drivers ({}) are configured",
					edt::vcs::valid_utf8(file), names));
			}
		}
		// check-attr reads the work tree's .gitattributes first, but checkout and merge smudge with the index's or the
		// target's: all of them are asked.
		std::vector<std::string> sources{ std::string{}, "--cached" };
		if (touched.kind == touched_kind::target && git.attr_source) sources.push_back("--source=" + touched.target);
		std::vector<std::string> applied;
		for (const auto& source : sources) {
			if (!applied_drivers(git, list->files, configured, source, applied, stop)) {
				return refuse(std::format("filter drivers ({}) are configured and git could not read the files' attributes", names));
			}
		}
		if (!applied.empty()) return refuse(std::format("the filter drivers ({}) apply to files this write touches", names_of(applied)));
		// Every git: an untracked .gitattributes plus a tracked one edited in the work tree can hide a driver from all
		// three views above.
		{
			const auto differ = attributes_differ_from_index(git, list->files, stop);
			if (!differ) return refuse(std::format("filter drivers ({}) are configured and git could not compare the "
				"work tree's .gitattributes with the index", names));
			if (*differ) return refuse(std::format("filter drivers ({}) are configured and a .gitattributes of the work "
				"tree differs from the index", names));
		}
		return std::nullopt;
	}

	struct scoped_entry
	{
		std::string scope;
		// Section and key lower case, the subsection as written.
		std::string key;
		std::string value;
		// False for a bare key (`[section] key`, no '='), which git reads as boolean true; `key =` has an empty value.
		bool has_value = false;
	};

	// `git config -z --show-scope|--show-origin --get-regexp|--list`: "<scope>\0<key>\n<value>\0" records; a bare key
	// is "<scope>\0<key>\0" (no newline), `key =` is "<scope>\0<key>\n\0".
	std::vector<scoped_entry> scoped_entries_of(std::string_view listing)
	{
		std::vector<scoped_entry> entries;
		const auto fields = split_nul(listing);
		for (std::size_t i = 0; i + 1 < fields.size(); i += 2) {
			const std::string& record = fields[i + 1];
			const std::size_t newline = record.find('\n');
			const bool has_value = newline != std::string::npos;
			entries.push_back({ fields[i], record.substr(0, newline), has_value ? record.substr(newline + 1) : std::string{},
				has_value });
		}
		return entries;
	}

	bool is_repository_scope(std::string_view scope)
	{
		return scope == "local" || scope == "worktree";
	}

	// The base of a url.<base>.insteadOf the repository may set for an agent: https://, ssh:// or scp-like user@host:.
	bool is_safe_rewrite(std::string_view base)
	{
		if (base.starts_with("https://") || base.starts_with("ssh://")) return true;
		const std::size_t at = base.find('@');
		const std::size_t colon = base.find(':');
		return at != std::string_view::npos && at > 0 && colon != std::string_view::npos && colon > at + 1
			&& base.substr(0, colon).find('/') == std::string_view::npos;
	}

	// An agent's network call to `remote` (resolved by resolve_network_target, never empty): refused
	// (unsafe_repository) when the repository's config picks a program it would run (a credential helper,
	// core.sshCommand, a URL rewrite to another transport, upload-pack / receive-pack); otherwise `git` gets the remote's
	// upload-pack and receive-pack forced. Nullopt: git may run.
	std::optional<git_output> network_refusal(git_context& git, const std::string& remote, std::stop_token stop)
	{
		if (!git.guard_network) return std::nullopt;
		if (remote.empty()) return refused("unsafe_repository", "the remote of an agent's network call was not resolved");
		if (std::any_of(remote.begin(), remote.end(), [](char c) { return c == '=' || static_cast<unsigned char>(c) < 0x20; })) {
			return refused("unsafe_repository", std::format("the remote '{}' cannot be guarded", edt::vcs::valid_utf8(remote)));
		}
		git_output listed = run_git(git, { "config", "-z", "--show-scope", "--get-regexp",
			"^(credential\\..*helper|core\\.sshcommand|url\\..*\\.(insteadof|pushinsteadof)|remote\\..*\\.(uploadpack|receivepack))$" },
			stop, k_read_cap);
		if (listing_failed(listed)) return listed;
		if (listed.truncated) return refused("unsafe_repository", "the repository's git config is too large to check");
		const std::string upload_key = "remote." + remote + ".uploadpack";
		const std::string receive_key = "remote." + remote + ".receivepack";
		for (const auto& entry : scoped_entries_of(listed.out)) {
			const bool helper = entry.key.starts_with("credential.") && entry.key.ends_with(".helper");
			// An empty helper only clears the list.
			if (helper && (!entry.has_value || !entry.value.empty()) && entry.scope != "system" && entry.scope != "global"
				&& !git.allow_repo_credential_helper) {
				return refused("unsafe_repository", "a credential helper set by the repository: an agent's network call "
					"runs only the system's or the user's");
			}
			if (entry.key == "core.sshcommand" && is_repository_scope(entry.scope)) {
				return refused("unsafe_repository", "core.sshCommand set by the repository");
			}
			const bool rewrite = entry.key.starts_with("url.")
				&& (entry.key.ends_with(".insteadof") || entry.key.ends_with(".pushinsteadof"));
			if (rewrite && is_repository_scope(entry.scope)) {
				const std::string_view base = std::string_view(entry.key).substr(4, entry.key.rfind('.') - 4);
				if (!is_safe_rewrite(base)) {
					return refused("unsafe_repository", std::format("a URL rewrite to '{}' set by the repository",
						edt::vcs::valid_utf8(base)));
				}
			}
			// Any remote's, not only this one's: a remote named by a URL rewrite or a later call is as hostile.
			const bool remote_key = entry.key.starts_with("remote.");
			if ((remote_key && entry.key.ends_with(".uploadpack") && entry.value != "git-upload-pack")
				|| (remote_key && entry.key.ends_with(".receivepack") && entry.value != "git-receive-pack")) {
				return refused("unsafe_repository", std::format("{} set to '{}'", edt::vcs::valid_utf8(entry.key),
					edt::vcs::valid_utf8(entry.value)));
			}
		}
		for (std::string config : { upload_key + "=git-upload-pack", receive_key + "=git-receive-pack" }) {
			git.config_args.emplace_back("-c");
			git.config_args.push_back(std::move(config));
		}
		return std::nullopt;
	}

	std::string lower_case(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) {
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		}
		return out;
	}

	// Whether a key of the repository's own config may stand for a guarded call. Section and key compare without
	// case (git lower-cases them), the subsection is not looked at. filter.*, credential.*helper and core.hooksPath
	// pass here and stay under their own rules (filter_refusal, network_refusal, the hooks override).
	bool repo_key_allowed(std::string_view key, bool allow_hooks)
	{
		const std::string lower = lower_case(key);
		const std::size_t first = lower.find('.');
		const std::size_t last = lower.rfind('.');
		if (first == std::string::npos || first == 0 || last + 1 >= lower.size()) return false;
		const std::string_view section = std::string_view(lower).substr(0, first);
		const std::string_view name = std::string_view(lower).substr(last + 1);
		const bool subsection = first != last;
		const auto one_of = [](std::string_view value, std::initializer_list<std::string_view> allowed) {
			return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
		};
		// lfs.* is data, except the keys that name a program git-lfs runs.
		if (section == "lfs") {
			const bool program = subsection
				&& ((lower.starts_with("lfs.customtransfer.") && one_of(name, { "path", "args" }))
					|| (lower.starts_with("lfs.extension.") && one_of(name, { "clean", "smudge" })));
			return !program;
		}
		// Not extensions.partialClone (or any other): a partial clone fetches missing objects behind network_refusal.
		if (section == "extensions") {
			return !subsection && one_of(name, { "objectformat", "refstorage", "worktreeconfig", "preciousobjects" });
		}
		if (one_of(section, { "index", "gc", "maintenance" })) return true;
		if (!subsection) {
			if (section == "core") {
				return one_of(name, { "repositoryformatversion", "filemode", "bare", "logallrefupdates", "ignorecase",
					"symlinks", "precomposeunicode", "autocrlf", "eol", "safecrlf", "longpaths", "sparsecheckout",
					"sparsecheckoutcone" })
					|| (name == "hookspath" && allow_hooks);
			}
			if (section == "user") return one_of(name, { "name", "email" });
			if (section == "credential") return name == "helper";
			return one_of(lower, { "init.defaultbranch", "pull.rebase", "pull.ff", "push.default", "fetch.prune",
				"submodule.active" });
		}
		if (section == "remote") {
			return one_of(name, { "url", "pushurl", "fetch", "tagopt", "prune", "followremotehead" });
		}
		// Never submodule.<name>.update: "!command" runs it.
		if (section == "submodule") return one_of(name, { "url", "active" });
		if (section == "branch") return one_of(name, { "remote", "merge", "rebase", "pushremote" });
		if (section == "credential") return name == "helper";
		return section == "filter";
	}

	// git_config_bool: a bare key is true; true/yes/on and false/no/off without case; an empty value is false; an integer
	// (with a k, m or g suffix) is true unless 0. What git would reject as no boolean counts as true here.
	bool config_bool(std::string_view value, bool has_value)
	{
		if (!has_value) return true;
		const std::string lower = lower_case(value);
		if (lower.empty() || lower == "false" || lower == "no" || lower == "off") return false;
		if (lower == "true" || lower == "yes" || lower == "on") return true;
		std::string_view digits = lower;
		if (!digits.empty() && (digits.back() == 'k' || digits.back() == 'm' || digits.back() == 'g')) digits.remove_suffix(1);
		std::int64_t number = 0;
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
		if (error == std::errc{} && end == digits.data() + digits.size()) return number != 0;
		return true;
	}

	// A guarded call without allow_any_repo_config: refused (unsafe_repository) when the repository's own config
	// (.git/config and the worktree's config.worktree; includes not followed, include.* itself is no allowed key) sets
	// a key outside the allowlist. Read before anything else of the call.
	std::optional<git_output> config_refusal(const git_context& git, std::stop_token stop)
	{
		// Read when the allowlist applies, and also under allow_any_repo_config for what no opt-in but its own lifts:
		// a partial clone before git 2.45, config-based hooks (hook.*) without allow_hooks.
		if (!git.guarded || (!git.check_repo_config && !git.refuse_partial_clone && git.allow_hooks)) return std::nullopt;
		git_output local = run_git(git, { "config", "--local", "--list", "-z", "--show-origin" }, stop, k_read_cap);
		if (listing_failed(local)) return local;
		if (local.truncated) return refused("unsafe_repository", "the repository's git config is too large to check");
		std::vector<scoped_entry> entries = scoped_entries_of(local.out);
		// config.worktree is read whatever extensions.worktreeConfig says (a bare `worktreeConfig` key is true to git),
		// as a file: `config --worktree` fails without the extension in a repository with several worktrees.
		const git_output where = run_git(git, { "rev-parse", "--git-path", "config.worktree" }, stop, k_read_cap);
		if (!succeeded(where) || trim(where.out).empty()) {
			return refused("unsafe_repository", "git could not name the worktree's config file");
		}
		fs::path worktree_file = path_of(trim(where.out));
		if (worktree_file.is_relative()) worktree_file = git.cwd / worktree_file;
		std::error_code ec;
		const fs::file_status status = fs::status(worktree_file, ec);
		if (status.type() != fs::file_type::not_found) {
			if (ec) {
				return refused("unsafe_repository", std::format("the worktree's config file '{}' cannot be checked: {}",
					edt::utf8_of(worktree_file), ec.message()));
			}
			const git_output worktree = run_git(git, { "config", "--file", edt::utf8_of(worktree_file), "--list", "-z",
				"--show-origin" }, stop, k_read_cap);
			if (!succeeded(worktree)) {
				return refused("unsafe_repository", std::format("the worktree's config file '{}' cannot be read",
					edt::utf8_of(worktree_file)));
			}
			for (auto& entry : scoped_entries_of(worktree.out)) entries.push_back(std::move(entry));
		}
		for (const auto& entry : entries) {
			const std::string lower = lower_case(entry.key);
			// --local --list does not follow includes, but git applies them: an included file could carry hook.* or
			// remote.*.promisor past the two checks below.
			if ((!git.allow_hooks || git.refuse_partial_clone)
				&& (lower.starts_with("include.") || lower.starts_with("includeif."))) {
				return refused("unsafe_repository", std::format("the repository's git config sets '{}' ({}): an include, "
					"whose keys a git write not made by a person cannot check", edt::vcs::valid_utf8(entry.key),
					edt::vcs::valid_utf8(entry.scope)));
			}
			// hook.<name>.command / .event run whatever core.hooksPath says.
			if (!git.allow_hooks && lower.starts_with("hook.")) {
				return refused("unsafe_repository", std::format("the repository's git config sets '{}' ({}): a config-based "
					"hook, which a git write not made by a person runs only when hooks are allowed for it",
					edt::vcs::valid_utf8(entry.key), edt::vcs::valid_utf8(entry.scope)));
			}
			// git 2.43 `clone --filter` writes only remote.<name>.promisor (and partialclonefilter), no extension.
			const bool promisor = lower.starts_with("remote.") && lower.ends_with(".promisor")
				&& config_bool(entry.value, entry.has_value);
			if (git.refuse_partial_clone && promisor) {
				return refused("unsafe_repository", std::format("the repository is a partial clone ('{}' is true, {}): lazy "
					"fetch can't be disabled before git 2.45, so a git write not made by a person is refused even though the "
					"project allows any repository config", edt::vcs::valid_utf8(entry.key), edt::vcs::valid_utf8(entry.scope)));
			}
			if (git.refuse_partial_clone && lower == "extensions.partialclone") {
				return refused("unsafe_repository", std::format("the repository is a partial clone ({}): lazy fetch "
					"can't be disabled before git 2.45, so a git write not made by a person is refused even though the "
					"project allows any repository config", edt::vcs::valid_utf8(entry.scope)));
			}
			if (!git.check_repo_config) continue;
			if (!repo_key_allowed(entry.key, git.allow_hooks)) {
				return refused("unsafe_repository", std::format("the repository's git config sets '{}' ({}), which a git "
					"write not made by a person does not accept unless the project allows any repository config",
					edt::vcs::valid_utf8(entry.key), edt::vcs::valid_utf8(entry.scope)));
			}
		}
		return std::nullopt;
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

	// failure_of for a write: some failures ("nothing to commit", a rejected push's summary) are told on stdout only.
	std::optional<json::value> write_failure_of(git_output out, const ds::task_context& task)
	{
		if (trim(out.err).empty() && out.process.exit_code != 0) {
			out.err = out.out.size() > k_stderr_cap ? out.out.substr(out.out.size() - k_stderr_cap) : out.out;
		}
		return failure_of(out, task);
	}

	// write_failure_of for a step that updates the work tree (checkout, merge): git may report a file it could not write
	// ("error: unable to read sha1 file of x", a partial clone without lazy fetch) and still exit 0.
	std::optional<json::value> tree_write_failure_of(const git_output& out, const ds::task_context& task)
	{
		if (auto failure = write_failure_of(out, task)) return failure;
		for (const std::string_view line : lines_of(out.err)) {
			if (line.starts_with("error:") || line.starts_with("fatal:")) {
				return error_json(edt::vcs::classify_error(out.err), summary_of(out.err, out.process.exit_code), out.err,
					out.process.exit_code);
			}
		}
		return std::nullopt;
	}

	// The job's answer for what filter_refusal or network_refusal returned: a refusal code, or git's own failure.
	json::value refusal_json(const git_output& out, const ds::task_context& task)
	{
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		return error_json("git_failed", "git could not check the repository's config", out.err, out.process.exit_code);
	}

	// A guarded network call to a remote the agent policy does not list (vcs.remotes): the job's answer, before git
	// writes anything. Nullopt: allowed.
	std::optional<json::value> remote_refusal(const git_context& git, const std::string& remote)
	{
		if (std::find(git.allowed_remotes.begin(), git.allowed_remotes.end(), remote) != git.allowed_remotes.end()) return std::nullopt;
		return error_json("denied", std::format("the agent policy does not allow the remote '{}' (vcs.remotes)",
			edt::vcs::valid_utf8(remote)), {}, -1);
	}

	// What a write that went well says: git's last lines.
	json::value output_json(const git_output& out)
	{
		std::string text = out.out;
		if (!text.empty() && !out.err.empty()) text.push_back('\n');
		text += out.err;
		return json::object{ { "output", json::string(stderr_tail(text)) } };
	}

	// A path, a ref or a remote as an argument: git must never read it as an option, and a NUL would cut it short.
	std::optional<std::string> argument_error(std::string_view what, std::string_view value)
	{
		if (value.empty()) return std::format("{} must not be empty", what);
		if (value.front() == '-') return std::format("{} must not start with '-'", what);
		if (value.find('\0') != std::string_view::npos) return std::format("{} must not hold a NUL", what);
		return std::nullopt;
	}

	// A guarded branch or ref name: no glob characters, which push and branch would expand or reject inconsistently.
	std::optional<std::string> pattern_error(std::string_view what, std::string_view value)
	{
		if (value.find_first_of("*?[") != std::string_view::npos) return std::format("{} must not hold '*', '?' or '['", what);
		return std::nullopt;
	}

	// A guarded branch name as `git check-ref-format --branch` would take it, and stricter: every rule git has for a
	// ref name, each component included, so a name never means another ref than it reads as.
	std::optional<std::string> branch_name_error(std::string_view what, std::string_view name)
	{
		if (auto error = argument_error(what, name)) return error;
		if (auto error = pattern_error(what, name)) return error;
		const auto bad = [what, name](std::string_view why) {
			return std::format("{} '{}' is not a valid branch name: {}", what, edt::vcs::valid_utf8(name), why);
		};
		if (name == "@") return bad("'@' alone");
		for (const char c : name) {
			const auto byte = static_cast<unsigned char>(c);
			if (byte < 0x20 || byte == 0x7f || c == ' ') return bad("a space or a control character");
			if (c == '~' || c == '^' || c == ':' || c == '\\') return bad("one of ~ ^ : \\");
		}
		if (name.find("..") != std::string_view::npos) return bad("'..'");
		if (name.find("@{") != std::string_view::npos) return bad("'@{'");
		if (name.find("//") != std::string_view::npos) return bad("'//'");
		if (name.front() == '/' || name.back() == '/') return bad("a leading or trailing '/'");
		if (name.back() == '.') return bad("a trailing '.'");
		std::size_t start = 0;
		while (start <= name.size()) {
			const std::size_t end = std::min(name.find('/', start), name.size());
			const std::string_view component = name.substr(start, end - start);
			if (component.starts_with('.')) return bad("a component that starts with '.'");
			if (component.ends_with(".lock")) return bad("a component that ends with '.lock'");
			start = end + 1;
		}
		return std::nullopt;
	}

	// An agent session id as the MCP server makes it (32 hex digits); anything else gets no trailer.
	bool is_trailer_session(std::string_view session)
	{
		if (session.empty() || session.size() > 64) return false;
		return std::all_of(session.begin(), session.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-'; });
	}

	// A guarded commit's message, already through strip_session_trailers, with the session's trailer when the session
	// is valid.
	std::string with_session_trailer(std::string message, const std::string& session)
	{
		if (!is_trailer_session(session)) return message;
		while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' || message.back() == '\t')) {
			message.pop_back();
		}
		message += "\n\nAgent-Session: " + session + "\n";
		return message;
	}

	// What a guarded network call talks to, resolved once and then named to git explicitly, so the check and git can
	// never see two different remotes (a checkout on the other io thread may move HEAD meanwhile).
	struct network_target
	{
		std::string remote;
		// The current branch; empty: HEAD is detached.
		std::string branch;
		// Not empty: git could not tell, the call is refused with this.
		std::string error;
	};

	// The remote as named, else the current branch's branch.<b>.remote, else the only configured remote, else origin:
	// the remote git itself would pick.
	network_target resolve_network_target(const git_context& git, const std::string& named, std::stop_token stop)
	{
		network_target out;
		const git_output head = run_git(git, { "symbolic-ref", "-q", "--short", "HEAD" }, stop, k_read_cap);
		// Exit code 1: HEAD is detached.
		if (listing_failed(head) || head.truncated) {
			out.error = "git could not tell the current branch";
			return out;
		}
		if (head.process.exit_code == 0) out.branch = std::string(trim(head.out));
		if (!named.empty()) {
			out.remote = named;
			return out;
		}
		if (!out.branch.empty()) {
			const git_output remote = run_git(git, { "config", "--get", "branch." + out.branch + ".remote" }, stop, k_read_cap);
			if (listing_failed(remote) || remote.truncated) {
				out.error = "git could not read the current branch's remote";
				return out;
			}
			if (remote.process.exit_code == 0 && !trim(remote.out).empty()) {
				out.remote = std::string(trim(remote.out));
				return out;
			}
		}
		const git_output remotes = run_git(git, { "remote" }, stop, k_read_cap);
		if (!succeeded(remotes)) {
			out.error = "git could not list the remotes";
			return out;
		}
		const std::vector<std::string_view> names = lines_of(remotes.out);
		out.remote = names.size() == 1 ? std::string(names.front()) : std::string("origin");
		return out;
	}

	// The upstream branch.<b>.merge names, as a guarded pull fetches it explicitly; nullopt with `error` set when there
	// is none or it is not a plain branch ref (a refspec with ':' or '+' would write local refs).
	std::optional<std::string> upstream_ref(const git_context& git, const std::string& branch, std::string& error,
		std::stop_token stop)
	{
		if (branch.empty()) {
			error = "HEAD is detached: name the remote's branch to pull";
			return std::nullopt;
		}
		const git_output merge = run_git(git, { "config", "--get", "branch." + branch + ".merge" }, stop, k_read_cap);
		const std::string ref(trim(merge.out));
		if (listing_failed(merge) || merge.truncated) {
			error = "git could not read the current branch's upstream";
			return std::nullopt;
		}
		if (merge.process.exit_code != 0 || ref.empty()) {
			error = std::format("the branch '{}' has no upstream: name the remote's branch to pull", edt::vcs::valid_utf8(branch));
			return std::nullopt;
		}
		if (!ref.starts_with("refs/") || branch_name_error("the upstream", ref)) {
			error = std::format("the upstream '{}' of the branch '{}' is not a plain ref", edt::vcs::valid_utf8(ref),
				edt::vcs::valid_utf8(branch));
			return std::nullopt;
		}
		return ref;
	}

	// The policy file as a path of the repository ('/'-separated): empty when it is not in this repository; nullopt when
	// git could not tell. Git computes it (rev-parse in the nearest folder of the file that exists), so a symlink or the
	// case of a folder name never makes the file look outside.
	std::optional<std::string> policy_path_in_repository(const git_context& git, std::stop_token stop)
	{
		if (git.policy_file.empty()) return std::string{};
		fs::path dir = git.policy_file.parent_path();
		fs::path rest = git.policy_file.filename();
		std::error_code ec;
		while (!dir.empty() && !fs::is_directory(dir, ec)) {
			if (dir == dir.parent_path()) return std::string{};
			rest = dir.filename() / rest;
			dir = dir.parent_path();
		}
		if (dir.empty()) return std::string{};
		const git_output own = run_git(git, { "rev-parse", "--show-toplevel" }, stop, k_read_cap);
		if (!succeeded(own)) return std::nullopt;
		git_context there = git;
		there.cwd = dir;
		const git_output where = run_git(there, { "rev-parse", "--show-toplevel", "--show-prefix" }, stop, k_read_cap);
		if (!where.process.error.empty() || where.truncated) return std::nullopt;
		if (where.process.exit_code != 0) {
			// Not in a repository at all: nothing a checkout here can change. Any other failure fails closed.
			if (std::string_view(edt::vcs::classify_error(where.err)) == "not_a_repository") return std::string{};
			return std::nullopt;
		}
		const std::size_t first_end = where.out.find('\n');
		if (first_end == std::string::npos) return std::nullopt;
		// Another repository (a nested one, or the file is outside this one).
		if (trim(std::string_view(where.out).substr(0, first_end)) != trim(own.out)) return std::string{};
		const std::string prefix(trim(std::string_view(where.out).substr(first_end + 1)));
		const std::u8string tail = rest.generic_u8string();
		return prefix + std::string(tail.begin(), tail.end());
	}

	// A guarded checkout or pull to `target` (a commit OID) that would change the project's agent policy file: an agent
	// must not change the policy it is judged by (agent_access reloads the file when it changes). Nullopt: allowed.
	std::optional<json::value> policy_refusal(const git_context& git, const std::string& target, const ds::task_context& task)
	{
		if (!git.guarded) return std::nullopt;
		const auto deny = [](std::string message) { return error_json("denied", message, {}, -1); };
		const std::optional<std::string> path = policy_path_in_repository(git, task.stop);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		if (!path) return deny("git could not tell where the agent policy file is in the repository");
		if (path->empty()) return std::nullopt;
		const std::string shown = edt::vcs::valid_utf8(*path);
		const git_output head = run_git(git, { "rev-parse", "--verify", "--quiet", "HEAD^{commit}" }, task.stop, k_read_cap);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		if (!head.process.error.empty() || head.truncated) return deny("git could not read HEAD");
		if (head.process.exit_code != 0) {
			// No commit yet: the target brings the file in if its tree has it.
			const git_output has = run_git(git, { "cat-file", "-e", target + ":" + *path }, task.stop, k_read_cap);
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			if (!has.process.error.empty() || has.process.exit_code == 0) {
				return deny(std::format("the target holds the agent policy file '{}': agents cannot change the policy that "
					"judges them", shown));
			}
			return std::nullopt;
		}
		const git_output diff = run_git(git, { "diff-tree", "--quiet", std::string(trim(head.out)), target, "--", *path },
			task.stop, k_read_cap);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		if (succeeded(diff)) return std::nullopt;
		if (diff.process.error.empty() && diff.process.exit_code == 1) {
			return deny(std::format("the target changes the agent policy file '{}': agents cannot change the policy that "
				"judges them", shown));
		}
		return deny(std::format("git could not compare the agent policy file '{}' with the target", shown));
	}

	// A guarded commit's message: every line that reads as an Agent-Session trailer goes (it is the editor's to write),
	// then the session's trailer is added when the session is one the MCP server makes.
	std::string strip_session_trailers(std::string_view message)
	{
		std::string out;
		out.reserve(message.size());
		while (!message.empty()) {
			const std::size_t end = message.find('\n');
			const std::string_view line = message.substr(0, end);
			message.remove_prefix(end == std::string_view::npos ? message.size() : end + 1);
			std::string_view token = line;
			while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
			constexpr std::string_view key = "agent-session";
			bool forged = token.size() >= key.size();
			for (std::size_t i = 0; forged && i < key.size(); ++i) {
				const char c = token[i];
				forged = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == key[i];
			}
			if (forged) {
				token.remove_prefix(key.size());
				while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
				forged = !token.empty() && token.front() == ':';
			}
			if (forged) continue;
			out.append(line);
			if (end != std::string_view::npos) out.push_back('\n');
		}
		return out;
	}

	// A remote is the name of a configured one: no URL (no ':'), so the transport is never one the call picks.
	std::optional<std::string> remote_error(std::string_view remote)
	{
		if (auto error = argument_error("remote", remote)) return error;
		if (remote.find(':') != std::string_view::npos) return std::string("remote must be the name of a configured remote, not a URL");
		return std::nullopt;
	}

	// A branch of pull or push: a plain name, never a refspec that forces (+x) or names another destination (a:b).
	std::optional<std::string> branch_error(std::string_view branch)
	{
		if (!branch.empty() && branch.front() == '+') return std::string("branch must not start with '+' (no force)");
		if (auto error = argument_error("branch", branch)) return error;
		if (branch.find(':') != std::string_view::npos) return std::string("branch must be a branch name, not a refspec with ':'");
		return std::nullopt;
	}

	std::optional<std::string> paths_error(const std::vector<std::string>& paths)
	{
		if (paths.empty()) return std::string("paths must not be empty");
		std::size_t bytes = 0;
		for (const auto& path : paths) {
			if (path.empty()) return std::string("a path must not be empty");
			if (path.find('\0') != std::string::npos) return std::string("a path must not hold a NUL");
			bytes += path.size() + 1;
		}
		if (bytes > edt::vcs_service::k_max_paths_bytes) {
			return std::format("the paths take more than {} bytes: give fewer per call", edt::vcs_service::k_max_paths_bytes);
		}
		return std::nullopt;
	}

	// Deletes the commit message file however the job ends.
	struct remove_on_exit
	{
		fs::path path;

		explicit remove_on_exit(fs::path file) : path(std::move(file)) {}
		remove_on_exit(const remove_on_exit&) = delete;
		remove_on_exit& operator=(const remove_on_exit&) = delete;
		~remove_on_exit()
		{
			std::error_code ec;
			fs::remove(path, ec);
		}
	};

	constexpr std::string_view k_no_hooks_dir = "no_hooks";

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

	struct log_page
	{
		// 0: the first page.
		std::uint64_t snapshot = 0;
		std::int64_t skip = 0;
	};

	// "" (the first page) or "s<snapshot>:<skip>", as vcs.log returns it.
	std::optional<log_page> page_of(std::string_view cursor)
	{
		if (cursor.empty()) return log_page{};
		const std::size_t colon = cursor.find(':');
		if (!cursor.starts_with('s') || colon == std::string_view::npos) return std::nullopt;
		log_page page;
		const std::string_view id = cursor.substr(1, colon - 1);
		const auto [id_end, id_error] = std::from_chars(id.data(), id.data() + id.size(), page.snapshot);
		if (id.empty() || id_error != std::errc{} || id_end != id.data() + id.size() || page.snapshot == 0) return std::nullopt;
		const std::string_view skip = cursor.substr(colon + 1);
		if (skip.empty() || skip.front() < '0' || skip.front() > '9') return std::nullopt;
		std::int64_t value = 0;
		const auto [end, error] = std::from_chars(skip.data(), skip.data() + skip.size(), value);
		if (end != skip.data() + skip.size()) return std::nullopt;
		if (error == std::errc::result_out_of_range) value = k_max_skip;
		else if (error != std::errc{}) return std::nullopt;
		page.skip = std::min(value, k_max_skip);
		return page;
	}

	// An author or grep filter of vcs.log.
	std::optional<std::string> filter_error(std::string_view what, std::string_view value)
	{
		if (value.size() > edt::vcs_service::k_max_filter_bytes) {
			return std::format("{} must be at most {} bytes", what, edt::vcs_service::k_max_filter_bytes);
		}
		const bool control = std::any_of(value.begin(), value.end(), [](char c) {
			const auto byte = static_cast<unsigned char>(c);
			return byte < 0x20 || byte == 0x7F;
		});
		if (control) return std::format("{} must not hold a control character", what);
		return std::nullopt;
	}

	// A ref of vcs.log: check-ref-format characters plus ~^@{} (HEAD~3, v1.0^{commit}, main@{1}). Never ':' nor "^{/",
	// with which git searches every commit message with a regex, nor a range.
	std::optional<std::string> ref_error(std::string_view ref)
	{
		if (ref.empty()) return std::nullopt;
		if (ref.front() == '-') return std::string("ref must not start with '-'");
		static constexpr std::string_view punctuation = "/._-+~^@{}#%,=";
		const bool allowed = std::all_of(ref.begin(), ref.end(), [](char c) {
			const auto byte = static_cast<unsigned char>(c);
			return byte >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
				|| punctuation.find(c) != std::string_view::npos;
		});
		if (!allowed) return std::string("ref holds a character no ref name holds (':', a space, a control character, ...)");
		if (ref.find("..") != std::string_view::npos) return std::string("ref must name one revision, not a range");
		if (ref.find("^{/") != std::string_view::npos) return std::string("ref must not search commit messages (^{/...})");
		return std::nullopt;
	}

	// A path of log or diff: no NUL and no control character.
	std::optional<std::string> path_error(std::string_view path)
	{
		const bool control = std::any_of(path.begin(), path.end(), [](char c) {
			const auto byte = static_cast<unsigned char>(c);
			return byte < 0x20 || byte == 0x7F;
		});
		if (control) return std::string("path must not hold a control character");
		return std::nullopt;
	}

	std::optional<std::string> commit_error(std::string_view commit)
	{
		if (!edt::vcs::is_hex_object_name(commit)) return std::string("commit must be 4 to 64 hex digits");
		return std::nullopt;
	}

	bool is_full_oid(std::string_view text)
	{
		return (text.size() == 40 || text.size() == 64) && edt::vcs::is_hex_object_name(text);
	}

	// What makes a snapshot's later pages the same query: every part with its length, so no two queries read alike.
	std::string snapshot_key(std::initializer_list<std::string_view> parts)
	{
		std::string key;
		for (const std::string_view part : parts) key.append(std::format("{}:{}|", part.size(), part));
		return key;
	}

	json::value stale_json()
	{
		return error_json("stale", "the history this cursor pages through is no longer kept; read it again from the first page",
			{}, -1);
	}

	// The job's answer when harden_read refused.
	json::value refused_read(const git_output& out, const ds::task_context& task)
	{
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		return error_json("git_failed", "git could not be prepared for a read", out.err, out.process.exit_code);
	}

	// failure_of for resolving a revision: whatever git says about one it cannot resolve ("Needed a single revision",
	// an ambiguous short name) is bad_revision.
	std::optional<json::value> revision_failure_of(const git_output& out, const ds::task_context& task)
	{
		auto failure = failure_of(out, task);
		if (failure) {
			json::object& error = failure->as_object()["error"].as_object();
			if (error["code"].as_string() == "git_failed") error["code"] = "bad_revision";
		}
		return failure;
	}

	// A revision that git cannot read as an option or a range, so rev-parse needs no --end-of-options for it.
	bool is_single_revision(std::string_view revision)
	{
		return !revision.empty() && revision.front() != '-' && revision.find("..") == std::string_view::npos;
	}

	// `rev-parse <options> [--end-of-options] <revisions>`: git before 2.30 does not know --end-of-options, and gets only
	// revisions is_single_revision passed.
	std::vector<std::string> rev_parse_args(const git_context& git, std::initializer_list<std::string_view> options,
		const std::vector<std::string>& revisions)
	{
		std::vector<std::string> args{ "rev-parse" };
		for (const std::string_view option : options) args.emplace_back(option);
		if (git.rev_parse_end_of_options) args.emplace_back("--end-of-options");
		args.insert(args.end(), revisions.begin(), revisions.end());
		return args;
	}

	// `revision` peeled to a commit, as its full object name.
	std::optional<json::value> resolve_commit(const git_context& git, std::string_view revision, const ds::task_context& task,
		std::string& oid)
	{
		if (!is_single_revision(revision)) {
			return error_json("bad_revision", "a revision must not start with '-' nor name a range", {}, -1);
		}
		const git_output out = run_git(git, rev_parse_args(git, { "--verify" }, { std::string(revision) + "^{commit}" }),
			task.stop, k_read_cap);
		if (auto failure = revision_failure_of(out, task)) return failure;
		oid = std::string(trim(out.out));
		if (!is_full_oid(oid)) return error_json("bad_revision", "git did not name a commit", out.err, out.process.exit_code);
		return std::nullopt;
	}

	// `commit` resolved, and its first parent (nullopt: a root commit).
	std::optional<json::value> resolve_with_parent(const git_context& git, std::string_view commit, const ds::task_context& task,
		std::string& oid, std::optional<std::string>& parent)
	{
		if (auto failure = resolve_commit(git, commit, task, oid)) return failure;
		const git_output first = run_git(git, rev_parse_args(git, { "--verify", "--quiet" }, { oid + "^1" }), task.stop, k_read_cap);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		if (first.process.error.empty() && first.process.exit_code == 0) {
			std::string found(trim(first.out));
			if (!is_full_oid(found)) return error_json("git_failed", "git did not name the first parent", first.err, 0);
			parent = std::move(found);
			return std::nullopt;
		}
		// --quiet: no parent is exit code 1 and nothing on stderr.
		if (first.process.error.empty() && first.process.exit_code == 1 && trim(first.err).empty()) return std::nullopt;
		return failure_of(first, task);
	}

	// A log over every branch, remote branch and tag names its tips on the command line (Windows holds 32K characters).
	constexpr std::size_t k_max_tip_bytes = 16 * 1024;
	constexpr std::size_t k_tip_chunk = 256;

	std::size_t bytes_of(const std::vector<std::string>& args)
	{
		std::size_t bytes = 0;
		for (const auto& arg : args) bytes += arg.size() + 1;
		return bytes;
	}

	// The commits of every branch, remote branch and tag; while they take too much of a command line, only those no
	// other one reaches (merge-base --independent), which walk the same history.
	std::optional<json::value> all_tips(const git_context& git, const ds::task_context& task, std::vector<std::string>& tips)
	{
		const git_output refs = run_git(git, { "for-each-ref", std::string(edt::vcs::k_tip_format), "refs/heads", "refs/remotes",
			"refs/tags" }, task.stop, k_read_cap);
		if (auto failure = failure_of(refs, task)) return failure;
		if (refs.truncated) return error_json("git_failed", "the repository has too many refs to list", {}, -1);
		edt::vcs::tip_list listed = edt::vcs::parse_tips(refs.out);
		tips = std::move(listed.commits);
		const auto add_tip = [&tips](std::string oid) {
			if (std::find(tips.begin(), tips.end(), oid) == tips.end()) tips.push_back(std::move(oid));
		};
		// A tag of a tag: one rev-parse per chunk; if one of them peels to no commit (a tag of a tree), each alone.
		for (std::size_t start = 0; start < listed.nested_tags.size(); start += k_tip_chunk) {
			const std::size_t end = std::min(listed.nested_tags.size(), start + k_tip_chunk);
			std::vector<std::string> peel;
			for (std::size_t i = start; i < end; ++i) peel.push_back(listed.nested_tags[i] + "^{commit}");
			const git_output batch = run_git(git, rev_parse_args(git, {}, peel), task.stop, k_read_cap);
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			const auto peeled = lines_of(batch.out);
			if (succeeded(batch) && peeled.size() == peel.size() && std::all_of(peeled.begin(), peeled.end(), is_full_oid)) {
				for (const std::string_view oid : peeled) add_tip(std::string(oid));
				continue;
			}
			for (const auto& one : peel) {
				const git_output out = run_git(git, rev_parse_args(git, { "--verify", "--quiet" }, { one }), task.stop, k_read_cap);
				if (task.stop.stop_requested()) throw ds::task_cancelled{};
				const std::string oid(trim(out.out));
				// git ran and found no commit under it (a tag of a tree): left out, like a plain tag of a tree.
				if (succeeded(out) && is_full_oid(oid)) add_tip(oid);
				else if (!out.process.error.empty()) return failure_of(out, task);
			}
		}
		while (bytes_of(tips) > k_max_tip_bytes) {
			std::vector<std::string> independent;
			for (std::size_t start = 0; start < tips.size(); start += k_tip_chunk) {
				const std::size_t end = std::min(tips.size(), start + k_tip_chunk);
				std::vector<std::string> args{ "merge-base", "--independent" };
				args.insert(args.end(), tips.begin() + static_cast<std::ptrdiff_t>(start), tips.begin() + static_cast<std::ptrdiff_t>(end));
				const git_output out = run_git(git, args, task.stop, k_read_cap);
				if (auto failure = failure_of(out, task)) return failure;
				for (const std::string_view line : lines_of(out.out)) {
					if (is_full_oid(line)) independent.emplace_back(line);
				}
			}
			std::sort(independent.begin(), independent.end());
			independent.erase(std::unique(independent.begin(), independent.end()), independent.end());
			if (independent.size() >= tips.size()) {
				return error_json("git_failed", "the branches and tags have too many separate tips for one log; name a ref",
					{}, -1);
			}
			tips = std::move(independent);
		}
		return std::nullopt;
	}

	// The text of a diff, cut at k_diff_cap.
	json::value diff_json(const git_output& out)
	{
		constexpr std::size_t cap = edt::vcs_service::k_diff_cap;
		std::string text = edt::vcs::valid_utf8(edt::vcs::utf8_prefix(out.out, cap));
		bool truncated = out.truncated;
		// Replacement characters may have made it longer than the cap.
		if (text.size() > cap) {
			text = std::string(edt::vcs::utf8_prefix(text, cap));
			truncated = true;
		}
		return json::object{ { "text", json::string(text) }, { "truncated", truncated } };
	}
}

// The tips of the first page of a log, kept for its later pages. Locked: the log jobs run on the io lane.
struct edt::vcs_service::log_snapshots
{
	struct snapshot
	{
		std::uint64_t id = 0;
		std::string key;
		std::vector<std::string> tips;
		std::uint64_t used = 0;
	};

	std::mutex mutex;
	std::vector<snapshot> items;
	std::uint64_t next_id = 1;
	std::uint64_t clock = 0;

	// Nullopt when the snapshot was dropped, or was made for another query.
	std::optional<std::vector<std::string>> find(std::uint64_t id, const std::string& key)
	{
		const std::scoped_lock lock(mutex);
		for (auto& item : items) {
			if (item.id != id) continue;
			if (item.key != key) return std::nullopt;
			item.used = ++clock;
			return item.tips;
		}
		return std::nullopt;
	}

	std::uint64_t add(std::string key, std::vector<std::string> tips)
	{
		const std::scoped_lock lock(mutex);
		if (items.size() >= k_max_log_snapshots) {
			items.erase(std::min_element(items.begin(), items.end(),
				[](const snapshot& a, const snapshot& b) { return a.used < b.used; }));
		}
		const std::uint64_t id = next_id++;
		items.push_back({ id, std::move(key), std::move(tips), ++clock });
		return id;
	}
};

template<class F>
edt::vcs_service::started edt::vcs_service::run_job(std::string label, F work, std::optional<write_mode> write, const hard_rule& rule)
{
	// Only "no git" and "no project" refuse at once; whatever git itself reports (not a repository, ...) is the job's
	// result, so every command answers the same way.
	const state& now = info();
	if (!m_git_found || !m_probed_project || now.code == "vcs_unavailable" || now.code == "no_project") return unavailable();
	git_context context = make_context(m_state.git, m_environment, now.root ? *now.root : *m_probed_project, m_options.run);
	context.rev_parse_end_of_options = version_at_least(m_state.git_version, 2, 30);
	// Everything not from a person (agent, test, game) takes the guarded path.
	const bool guarded = write && write->caller != cmd::origin_kind::ui;
	// The network checks read the config with --show-scope (git 2.26).
	if (guarded && write->network && !version_at_least(m_state.git_version, 2, 26)) {
		return { 0, "vcs_unavailable", std::format("git 2.26 or newer is required for agent network operations (found {})",
			m_state.git_version) };
	}
	vcs_policy policy;
	std::optional<std::string> denied;
	if (guarded) {
		// Read once per job: an edit of the policy file applies to the next write.
		policy = current_policy();
		if (rule) denied = rule(policy);
	}
	if (denied) {
		const auto id = m_jobs.start_with_result(ds::lane::io, ds::task_desc{ std::move(label), "vcs" }, m_life,
			[reason = std::move(*denied)](ds::task_context&) -> json::value { return error_json("denied", reason, {}, -1); });
		return { id, {}, {} };
	}
	if (guarded) {
		const vcs_write_permissions& permissions = policy.permissions;
		std::erase_if(context.environment, [](const std::string& entry) { return is_askpass_variable(entry); });
		// The environment's GIT_SSH_COMMAND (always set) wins over core.sshCommand anyway; this states it.
		const std::string ssh = variable_of(context.environment, "GIT_SSH_COMMAND").value_or("ssh -oBatchMode=yes");
		context.config_args = { "-c", "core.askPass=", "-c", "core.sshCommand=" + ssh,
			// Only https and ssh: file:// and local paths run the other repository's hooks (git drops the -c overrides
			// for the local transport), git://, ext:: and <helper>:: run programs or go unencrypted.
			"-c", "protocol.allow=never", "-c", "protocol.https.allow=always", "-c", "protocol.ssh.allow=always",
			"-c", "protocol.ext.allow=never",
			// A repository's protocol.file.allow beats protocol.allow; the specific -c beats the repository's.
			"-c", "protocol.file.allow=never", "-c", "protocol.git.allow=never",
			"-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", "-c", "merge.verifySignatures=false",
			"-c", "gpg.program=", "-c", "gpg.openpgp.program=", "-c", "gpg.x509.program=", "-c", "gpg.ssh.program=",
			"-c", "gpg.ssh.defaultKeyCommand=",
			"-c", "core.gitProxy=", "-c", "core.alternateRefsCommand=",
			// No background gc or maintenance started by the write (they outlive the job and its overrides).
			"-c", "gc.auto=0", "-c", "maintenance.auto=false" };
		// The first core.gitProxy git reads wins, so the repository's would come before the -c one; an empty
		// GIT_PROXY_COMMAND is read before any config and means no proxy.
		set_variable(context.environment, "GIT_PROXY_COMMAND", "");
		// A partial clone fetches a missing object from its promisor remote on demand, past network_refusal. Honoured
		// by git 2.45+ and by maintenance releases back to 2.39.4 (distro 2.43 too); config_refusal covers the rest.
		set_variable(context.environment, "GIT_NO_LAZY_FETCH", "1");
		context.attr_source = version_at_least(m_state.git_version, 2, 40);
		if (!permissions.allow_hooks) {
			if (auto error = ensure_scratch()) return { 0, cmd::codes::failed, std::move(*error) };
			context.config_args.emplace_back("-c");
			context.config_args.push_back("core.hooksPath=" + utf8_of(m_scratch.root / k_no_hooks_dir));
		}
		context.guarded = true;
		context.check_repo_config = !permissions.allow_any_repo_config;
		context.refuse_partial_clone = permissions.allow_any_repo_config
			&& !version_at_least(m_state.git_version, 2, 45);
		context.allow_hooks = permissions.allow_hooks;
		context.refuse_filters = write->runs_filters && !permissions.allow_filters;
		context.guard_network = write->network;
		context.allow_repo_credential_helper = permissions.allow_repo_credential_helper;
		context.allowed_remotes = policy.remotes;
		if (!m_policy_file.empty()) context.policy_file = m_policy_file;
		else if (m_probed_project) context.policy_file = *m_probed_project / ".snk" / "agent_policy.json";
	}
	const auto id = m_jobs.start_with_result(ds::lane::io, ds::task_desc{ std::move(label), "vcs" }, m_life,
		[context = std::move(context), work = std::move(work)](ds::task_context& task) -> json::value {
			// The repository's own config is read before anything else of a guarded call.
			if (auto refusal = config_refusal(context, task.stop)) return refusal_json(*refusal, task);
			return work(context, task);
		});
	return { id, {}, {} };
}

edt::vcs_service::vcs_service(job_service& jobs, vcs_options options)
	: m_jobs(jobs)
	, m_options(std::move(options))
	, m_log_snapshots(std::make_shared<log_snapshots>())
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
	const git_output version = run_probe(make_context(git, m_environment, {}, m_options.run), { "--version" }, m_options.probe_timeout,
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
	const git_output top = run_probe(make_context(m_state.git, m_environment, *project, m_options.run), { "rev-parse", "--show-toplevel" },
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
	if (auto error = path_error(path)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	std::vector<std::string> args{ "diff", "--no-color", "--no-ext-diff", "--no-textconv", "--ignore-submodules=all" };
	if (staged) args.emplace_back("--cached");
	if (!path.empty()) {
		args.emplace_back("--");
		args.push_back(path);
	}
	return run_job("git diff", [args = std::move(args)](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_read(git, args, task.stop, k_diff_cap);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		return diff_json(out);
	});
}

edt::vcs_service::started edt::vcs_service::start_commit_diff(const std::string& commit, const std::string& path)
{
	if (auto error = commit_error(commit)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (auto error = path_error(path)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	return run_job("git diff", [commit, path](const git_context& git, ds::task_context& task) -> json::value {
		git_context read = git;
		if (auto refused = harden_read(read, task.stop)) return refused_read(*refused, task);
		std::string oid;
		std::optional<std::string> parent;
		if (auto failure = resolve_with_parent(read, commit, task, oid, parent)) return std::move(*failure);
		std::vector<std::string> args{ "diff-tree", "-p", "--no-color", "--no-ext-diff", "--no-textconv", "-M",
			"--ignore-submodules=all", "--no-commit-id" };
		if (parent) args.push_back(*parent);
		else args.emplace_back("--root");
		args.push_back(oid);
		args.emplace_back("--");
		if (!path.empty()) args.push_back(path);
		const git_output out = run_git(read, args, task.stop, k_diff_cap);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		return diff_json(out);
	});
}

edt::vcs_service::started edt::vcs_service::start_log(const std::string& ref, const std::string& path, int limit,
	const std::string& cursor, const log_filter& filter)
{
	if (auto error = ref_error(ref)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (auto error = path_error(path)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (filter.all && !ref.empty()) return { 0, cmd::codes::invalid_args, "all and ref cannot be given together" };
	if (auto error = filter_error("author", filter.author)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (auto error = filter_error("grep", filter.grep)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	const auto page = page_of(cursor);
	if (!page) return { 0, cmd::codes::invalid_args, "cursor is not one that vcs.log returned" };
	const int count = std::clamp(limit, 1, k_max_log_limit);
	// One more than asked: whether it comes back tells if there is a next page.
	std::vector<std::string> options{ "log", "-z", std::string(vcs::k_log_format), "--encoding=UTF-8", "--decorate=full",
		"--date-order", std::format("--skip={}", page->skip), std::format("--max-count={}", count + 1) };
	if (!filter.author.empty()) options.push_back("--author=" + filter.author);
	if (!filter.grep.empty()) options.push_back("--grep=" + filter.grep);
	if (!filter.author.empty() || !filter.grep.empty()) {
		options.emplace_back("--fixed-strings");
		options.emplace_back("-i");
	}
	std::string key = snapshot_key({ filter.all ? "all" : "ref", ref, path, filter.author, filter.grep });
	return run_job("git log", [options = std::move(options), key = std::move(key), ref, path, all = filter.all, count,
		page = *page, snapshots = m_log_snapshots](const git_context& git, ds::task_context& task) -> json::value {
		// The repository too: a snapshot of one project is never served for another.
		const std::string scoped_key = snapshot_key({ key, utf8_of(git.cwd) });
		git_context read = git;
		if (auto refused = harden_read(read, task.stop)) return refused_read(*refused, task);
		std::vector<std::string> tips;
		if (page.snapshot != 0) {
			auto kept = snapshots->find(page.snapshot, scoped_key);
			if (!kept) return stale_json();
			tips = std::move(*kept);
		} else if (all) {
			if (auto failure = all_tips(read, task, tips)) return std::move(*failure);
		} else {
			std::string oid;
			if (auto failure = resolve_commit(read, ref.empty() ? std::string_view("HEAD") : std::string_view(ref), task, oid)) {
				// A branch without commits has no log, which is not an error for the reader.
				const std::string_view code = failure->as_object().at("error").as_object().at("code").as_string();
				if (ref.empty() && (code == "bad_revision" || code == "no_commits")) {
					return json::object{ { "commits", json::array{} }, { "truncated", false } };
				}
				return std::move(*failure);
			}
			tips.push_back(std::move(oid));
		}
		if (tips.empty()) return json::object{ { "commits", json::array{} }, { "truncated", false } };
		std::vector<std::string> args = options;
		// Whatever the path holds, git reads it as a path, never as an option; the tips are object names.
		args.emplace_back("--end-of-options");
		args.insert(args.end(), tips.begin(), tips.end());
		args.emplace_back("--");
		if (!path.empty()) args.push_back(path);
		const git_output out = run_git(read, args, task.stop, k_read_cap);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		auto commits = vcs::parse_log(whole_records(out, '\0'));
		const bool more = out.truncated || commits.size() > static_cast<std::size_t>(count);
		if (commits.size() > static_cast<std::size_t>(count)) commits.resize(static_cast<std::size_t>(count));
		json::array entries;
		entries.reserve(commits.size());
		for (const auto& commit : commits) entries.push_back(vcs::to_json(commit));
		json::object result{ { "commits", std::move(entries) }, { "truncated", out.truncated } };
		if (more) {
			const std::uint64_t id = page.snapshot != 0 ? page.snapshot : snapshots->add(scoped_key, std::move(tips));
			result["next_cursor"] = json::string(std::format("s{}:{}", id, page.skip + static_cast<std::int64_t>(commits.size())));
		}
		return result;
	});
}

edt::vcs_service::started edt::vcs_service::start_show_files(const std::string& commit)
{
	if (auto error = commit_error(commit)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	return run_job("git show files", [commit](const git_context& git, ds::task_context& task) -> json::value {
		git_context read = git;
		if (auto refused = harden_read(read, task.stop)) return refused_read(*refused, task);
		std::string oid;
		std::optional<std::string> parent;
		if (auto failure = resolve_with_parent(read, commit, task, oid, parent)) return std::move(*failure);
		// A merge against its first parent: what the merge brought into the branch.
		std::vector<std::string> args{ "diff-tree", "-r", "-z", "--name-status", "-M", "--no-ext-diff", "--no-textconv",
			"--no-commit-id" };
		if (parent) args.push_back(*parent);
		else args.emplace_back("--root");
		args.push_back(oid);
		const git_output out = run_git(read, args, task.stop, k_read_cap);
		if (auto failure = failure_of(out, task)) return std::move(*failure);
		json::array files;
		for (const auto& file : vcs::parse_name_status(whole_records(out, '\0'))) files.push_back(vcs::to_json(file));
		return json::object{ { "files", std::move(files) }, { "truncated", out.truncated } };
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

edt::vcs_service::scratch_dir::~scratch_dir()
{
	if (root.empty()) return;
	std::error_code ec;
	fs::remove_all(root, ec);
}

std::optional<std::string> edt::vcs_service::ensure_scratch()
{
	std::error_code ec;
	if (!m_scratch.root.empty() && fs::is_directory(m_scratch.root, ec)) {
		const fs::path hooks = m_scratch.root / k_no_hooks_dir;
		if (fs::is_directory(hooks, ec) && fs::is_empty(hooks, ec) && !ec) return std::nullopt;
		// Something put a file in it (or took it away): an empty one again.
		fs::remove_all(hooks, ec);
		ec.clear();
		if (fs::create_directory(hooks, ec) && fs::is_empty(hooks, ec) && !ec) return std::nullopt;
		return std::format("cannot make the empty hooks folder '{}': {}", utf8_of(hooks), ec ? ec.message() : "not empty");
	}
	const fs::path temp = fs::temp_directory_path(ec);
	if (ec) return std::format("no temp folder for git's files: {}", ec.message());
	// Unguessable, and made by this call (create_directory fails on one that exists): nobody else put hooks in it.
	std::random_device entropy;
	for (int attempt = 0; attempt < 16; ++attempt) {
		const std::uint64_t tag = (std::uint64_t{ entropy() } << 32) ^ std::uint64_t{ entropy() }
			^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		const fs::path root = temp / std::format("snk_editor_vcs_{:016x}", tag);
		if (!fs::create_directory(root, ec)) {
			if (ec) return std::format("cannot make a folder for git's files in '{}': {}", utf8_of(temp), ec.message());
			continue;
		}
#ifndef _WIN32
		fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
		if (!ec) static_cast<void>(fs::create_directory(root / k_no_hooks_dir, ec));
		if (ec) {
			const std::string message = ec.message();
			fs::remove_all(root, ec);
			return std::format("cannot prepare the folder '{}' for git's files: {}", utf8_of(root), message);
		}
		m_scratch.root = root;
		return std::nullopt;
	}
	return std::format("cannot make a folder for git's files in '{}'", utf8_of(temp));
}

void edt::vcs_service::set_write_permissions(vcs_write_permissions permissions)
{
	m_write_permissions = permissions;
}

edt::vcs_policy edt::vcs_service::current_policy() const
{
	if (m_policy_source) return m_policy_source();
	vcs_policy defaults;
	defaults.permissions = m_write_permissions;
	return defaults;
}

bool edt::vcs_glob_match(std::string_view pattern, std::string_view text, bool ignore_case)
{
	const auto fold = [ignore_case](char c) {
		return ignore_case && c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
	};
	// Iterative, with one backtrack point per '*': linear enough for branch names, no recursion depth.
	std::size_t p = 0;
	std::size_t t = 0;
	std::size_t star = std::string_view::npos;
	std::size_t resume = 0;
	while (t < text.size()) {
		if (p < pattern.size() && pattern[p] == '*') {
			star = p++;
			resume = t;
		}
		else if (p < pattern.size() && (pattern[p] == '?' || fold(pattern[p]) == fold(text[t]))) {
			++p;
			++t;
		}
		else if (star != std::string_view::npos) {
			p = star + 1;
			t = ++resume;
		}
		else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == '*') ++p;
	return p == pattern.size();
}

edt::vcs_service::started edt::vcs_service::start_stage(const std::vector<std::string>& paths, const cmd::origin& caller)
{
	if (auto error = paths_error(paths)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	std::vector<std::string> args{ "add", "--" };
	args.insert(args.end(), paths.begin(), paths.end());
	return run_job("git add", [args = std::move(args), paths](const git_context& git, ds::task_context& task) -> json::value {
		if (auto refusal = filter_refusal(git, touched_files{ touched_kind::paths, paths, {} }, task.stop)) {
			return refusal_json(*refusal, task);
		}
		const git_output out = run_write(git, args, task.stop);
		if (auto failure = write_failure_of(out, task)) return std::move(*failure);
		return output_json(out);
	}, write_mode{ caller.kind, true, false });
}

edt::vcs_service::started edt::vcs_service::start_unstage(const std::vector<std::string>& paths, const cmd::origin& caller)
{
	if (auto error = paths_error(paths)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	// reset (not restore --staged): it also unstages on a branch without commits.
	std::vector<std::string> args{ "reset", "-q", "--" };
	args.insert(args.end(), paths.begin(), paths.end());
	return run_job("git reset", [args = std::move(args), paths](const git_context& git, ds::task_context& task) -> json::value {
		if (auto refusal = filter_refusal(git, touched_files{ touched_kind::paths, paths, {} }, task.stop)) {
			return refusal_json(*refusal, task);
		}
		const git_output out = run_write(git, args, task.stop);
		if (auto failure = write_failure_of(out, task)) return std::move(*failure);
		return output_json(out);
	}, write_mode{ caller.kind, true, false });
}

edt::vcs_service::started edt::vcs_service::start_commit(const std::string& message, const cmd::origin& caller)
{
	if (trim(message).empty()) return { 0, cmd::codes::invalid_args, "message must not be empty" };
	if (message.find('\0') != std::string::npos) return { 0, cmd::codes::invalid_args, "message must not hold a NUL" };
	std::string text = message;
	if (caller.kind != cmd::origin_kind::ui) {
		text = strip_session_trailers(message);
		if (trim(text).empty()) return { 0, cmd::codes::invalid_args, "message must not be empty (an Agent-Session line is the editor's)" };
		text = with_session_trailer(std::move(text), caller.session);
	}
	// The runner has no stdin: the message goes through a file only this editor writes to.
	if (auto error = ensure_scratch()) return { 0, cmd::codes::failed, std::move(*error) };
	const fs::path file = m_scratch.root / std::format("commit_message_{}.txt", ++m_message_count);
	return run_job("git commit", [file, message = std::move(text)](const git_context& git,
		ds::task_context& task) -> json::value {
		const remove_on_exit cleanup{ file };
		if (auto refusal = filter_refusal(git, touched_files{ touched_kind::index, {}, {} }, task.stop)) {
			return refusal_json(*refusal, task);
		}
		{
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			out.write(message.data(), static_cast<std::streamsize>(message.size()));
			out.close();
			if (!out) {
				return error_json("git_failed", std::format("cannot write the commit message to '{}'", utf8_of(file)), {}, -1);
			}
		}
		const git_output out = run_write(git, { "commit", "-q", "-F", utf8_of(file) }, task.stop);
		if (auto failure = write_failure_of(out, task)) return std::move(*failure);
		const git_output head = run_git(git, { "rev-parse", "--verify", "HEAD" }, task.stop);
		if (auto failure = failure_of(head, task)) return std::move(*failure);
		return json::object{ { "hash", json::string(vcs::valid_utf8(trim(head.out))) } };
	}, write_mode{ caller.kind, true, false });
}

edt::vcs_service::started edt::vcs_service::start_branch_create(const std::string& name, const std::string& from,
	const cmd::origin& caller)
{
	if (auto error = argument_error("name", name)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (!from.empty()) {
		if (auto error = argument_error("from", from)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	if (caller.kind != cmd::origin_kind::ui) {
		if (auto error = branch_name_error("name", name)) return { 0, cmd::codes::invalid_args, std::move(*error) };
		if (auto error = pattern_error("from", from)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	std::vector<std::string> args{ "branch", "--end-of-options", name };
	if (!from.empty()) args.push_back(from);
	return run_job("git branch", [args = std::move(args)](const git_context& git, ds::task_context& task) -> json::value {
		const git_output out = run_write(git, args, task.stop);
		if (auto failure = write_failure_of(out, task)) return std::move(*failure);
		return output_json(out);
	}, write_mode{ caller.kind, false, false });
}

edt::vcs_service::started edt::vcs_service::start_checkout(const std::string& ref, const cmd::origin& caller)
{
	if (auto error = argument_error("ref", ref)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (caller.kind != cmd::origin_kind::ui) {
		if (auto error = pattern_error("ref", ref)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	return run_job("git checkout", [ref](const git_context& git, ds::task_context& task) -> json::value {
		// The trailing "--": the ref is never read as a path. Never --force: local changes stop the checkout.
		std::vector<std::string> args{ "checkout", "-q", "--no-recurse-submodules", ref, "--" };
		std::string upstream;
		if (git.guarded) {
			// Resolved once: the filter check and the checkout must see the same tree.
			const auto plan = plan_checkout(git, ref, task.stop);
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			if (!plan) {
				return error_json("bad_revision", std::format("'{}' is no local branch, no commit and no branch of exactly "
					"one remote", vcs::valid_utf8(ref)), {}, -1);
			}
			std::string target_oid = plan->target;
			if (plan->kind == checkout_kind::remote_branch) {
				const git_output peeled = run_git(git, { "rev-parse", "--verify", "--quiet", plan->target + "^{commit}" },
					task.stop, k_read_cap);
				if (task.stop.stop_requested()) throw ds::task_cancelled{};
				if (!succeeded(peeled) || trim(peeled.out).empty()) {
					return error_json("bad_revision", std::format("'{}' is not a commit", vcs::valid_utf8(plan->target)), {}, -1);
				}
				target_oid = std::string(trim(peeled.out));
			}
			if (auto refusal = policy_refusal(git, target_oid, task)) return std::move(*refusal);
			if (auto refusal = filter_refusal(git, touched_files{ touched_kind::target, {}, target_oid }, task.stop)) {
				return refusal_json(*refusal, task);
			}
			// A branch by its short name, once refs/heads/<ref> is known to exist: `checkout refs/heads/x` would detach,
			// and with a branch x git takes the branch even when a tag x exists too.
			if (plan->kind == checkout_kind::commit) {
				args = { "checkout", "-q", "--no-recurse-submodules", "--detach", plan->target, "--" };
			}
			else if (plan->kind == checkout_kind::remote_branch) {
				// The checked OID, not the remote ref, which a fetch on the other io thread may move meanwhile; the
				// upstream is set after.
				args = { "checkout", "-q", "--no-recurse-submodules", "-b", ref, target_oid, "--" };
				upstream = plan->target;
			}
		}
		const git_output out = run_write(git, args, task.stop);
		if (auto failure = tree_write_failure_of(out, task)) return std::move(*failure);
		if (!upstream.empty()) {
			// The branch stays when this fails: the checkout is done, only the tracking is missing.
			const git_output tracked = run_write(git, { "branch", "--set-upstream-to=" + upstream, ref }, task.stop);
			if (auto failure = write_failure_of(tracked, task)) return std::move(*failure);
		}
		return output_json(out);
	}, write_mode{ caller.kind, true, false });
}

edt::vcs_service::started edt::vcs_service::start_fetch(const std::string& remote, const cmd::origin& caller)
{
	if (!remote.empty()) {
		if (auto error = remote_error(remote)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	return run_job("git fetch", [remote](const git_context& shared, ds::task_context& task) -> json::value {
		git_context git = shared;
		std::vector<std::string> args{ "fetch", "--no-recurse-submodules", "--end-of-options" };
		std::string target = remote;
		if (git.guarded) {
			// Resolved once and named to git: the remote checked is the remote fetched from.
			const network_target resolved = resolve_network_target(git, remote, task.stop);
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			if (!resolved.error.empty()) return error_json("git_failed", resolved.error, {}, -1);
			if (auto refusal = remote_refusal(git, resolved.remote)) return std::move(*refusal);
			target = resolved.remote;
		}
		if (auto refusal = network_refusal(git, target, task.stop)) return refusal_json(*refusal, task);
		if (!target.empty()) args.push_back(target);
		const git_output out = run_write(git, args, task.stop);
		if (auto failure = write_failure_of(out, task)) return std::move(*failure);
		return output_json(out);
	}, write_mode{ caller.kind, false, true });
}

edt::vcs_service::started edt::vcs_service::start_pull(const std::string& remote, const std::string& branch,
	const cmd::origin& caller)
{
	if (!branch.empty() && remote.empty()) return { 0, cmd::codes::invalid_args, "branch needs a remote" };
	if (!remote.empty()) {
		if (auto error = remote_error(remote)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	if (!branch.empty()) {
		if (auto error = branch_error(branch)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	// git pull --ff-only in steps, so the filter check sees what the merge would change: fetch, then a fast-forward
	// to what fetch marked for merging. Never a rebase (whatever pull.rebase says) nor a merge commit.
	return run_job("git pull", [remote, branch](const git_context& shared, ds::task_context& task) -> json::value {
		git_context git = shared;
		std::vector<std::string> args{ "fetch", "--no-recurse-submodules", "--end-of-options" };
		std::string target = remote;
		std::string wanted = branch;
		network_target resolved;
		if (git.guarded) {
			// The remote and the branch to merge are resolved once and named to git, so neither a checkout meanwhile
			// nor git's own defaults can make it fetch from another remote than the one checked.
			resolved = resolve_network_target(git, remote, task.stop);
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			if (!resolved.error.empty()) return error_json("git_failed", resolved.error, {}, -1);
			if (auto refusal = remote_refusal(git, resolved.remote)) return std::move(*refusal);
			target = resolved.remote;
		}
		if (auto refusal = network_refusal(git, target, task.stop)) return refusal_json(*refusal, task);
		if (git.guarded && wanted.empty() && !remote.empty()) {
			// A named remote without a branch: only the current branch's upstream on that very remote is pulled, never
			// its branch.<b>.merge fetched from a remote it does not track.
			std::string tracked;
			if (!resolved.branch.empty()) {
				const git_output own = run_git(git, { "config", "--get", "branch." + resolved.branch + ".remote" }, task.stop,
					k_read_cap);
				if (task.stop.stop_requested()) throw ds::task_cancelled{};
				if (listing_failed(own) || own.truncated) return error_json("git_failed", "git could not read the current branch's remote", {}, -1);
				tracked = std::string(trim(own.out));
			}
			if (tracked != remote) {
				return error_json(cmd::codes::invalid_args, std::format("name the branch to pull from '{}'",
					vcs::valid_utf8(remote)), {}, -1);
			}
		}
		if (git.guarded && wanted.empty()) {
			std::string why;
			const std::optional<std::string> upstream = upstream_ref(git, resolved.branch, why, task.stop);
			if (task.stop.stop_requested()) throw ds::task_cancelled{};
			if (!upstream) return error_json("git_failed", why, {}, -1);
			wanted = *upstream;
		}
		if (!target.empty()) args.push_back(target);
		if (!wanted.empty()) args.push_back(wanted);
		const git_output fetched = run_write(git, args, task.stop);
		if (auto failure = write_failure_of(fetched, task)) return std::move(*failure);
		const git_output where = run_git(git, { "rev-parse", "--git-path", "FETCH_HEAD" }, task.stop, k_read_cap);
		if (auto failure = failure_of(where, task)) return std::move(*failure);
		fs::path fetch_head = path_of(trim(where.out));
		if (fetch_head.is_relative()) fetch_head = git.cwd / fetch_head;
		std::string first;
		{
			std::ifstream in(fetch_head, std::ios::binary);
			std::getline(in, first);
		}
		// FETCH_HEAD lists what is for merging first; nothing is when the branch has no upstream and none was named.
		if (trim(first).empty() || first.find("\tnot-for-merge\t") != std::string::npos) {
			return error_json("git_failed", "there is nothing to merge: name the remote's branch, or set the current "
				"branch's upstream", fetched.err, -1);
		}
		// Its object, as a commit OID read once: the check and the merge must see the same commit, whatever writes
		// FETCH_HEAD meanwhile.
		const std::string fetched_object(trim(std::string_view(first).substr(0, first.find('\t'))));
		const git_output peeled = run_git(git, { "rev-parse", "--verify", "--quiet", fetched_object + "^{commit}" },
			task.stop, k_read_cap);
		if (task.stop.stop_requested()) throw ds::task_cancelled{};
		const std::string commit(trim(peeled.out));
		if (fetched_object.empty() || !succeeded(peeled) || commit.empty()) {
			return error_json("git_failed", "what fetch got for merging is not a commit", fetched.err, -1);
		}
		if (auto refusal = policy_refusal(git, commit, task)) return std::move(*refusal);
		if (auto refusal = filter_refusal(git, touched_files{ touched_kind::target, {}, commit }, task.stop)) {
			return refusal_json(*refusal, task);
		}
		const git_output merged = run_write(git, { "-c", "submodule.recurse=false", "merge", "--ff-only", "--no-stat",
			commit }, task.stop);
		if (auto failure = tree_write_failure_of(merged, task)) return std::move(*failure);
		git_output both = merged;
		both.out = fetched.out + merged.out;
		both.err = fetched.err + merged.err;
		return output_json(both);
	}, write_mode{ caller.kind, true, true });
}

edt::vcs_service::started edt::vcs_service::start_push(const std::string& remote, const std::string& branch,
	bool set_upstream, const cmd::origin& caller)
{
	if (auto error = remote_error(remote)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	if (auto error = branch_error(branch)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	const bool guarded = caller.kind != cmd::origin_kind::ui;
	if (guarded) {
		if (auto error = branch_name_error("branch", branch)) return { 0, cmd::codes::invalid_args, std::move(*error) };
	}
	// Never --force, --force-with-lease, --mirror or a +refspec: a push that is not a fast-forward is rejected.
	std::vector<std::string> args{ "push", "--recurse-submodules=no" };
	// No tags, and the destination spelled out: push.default, remote.*.push and branch.*.merge cannot send a guarded
	// push anywhere but the branch the hard rule checked.
	if (guarded) args.emplace_back("--no-follow-tags");
	if (set_upstream) args.emplace_back("--set-upstream");
	args.emplace_back("--end-of-options");
	args.push_back(remote);
	args.push_back(guarded ? std::format("refs/heads/{0}:refs/heads/{0}", branch) : branch);
	const hard_rule rule = [branch](const vcs_policy& policy) -> std::optional<std::string> {
		const auto matches = [&branch](const std::vector<std::string>& patterns, bool ignore_case) {
			return std::any_of(patterns.begin(), patterns.end(),
				[&branch, ignore_case](const std::string& pattern) { return vcs_glob_match(pattern, branch, ignore_case); });
		};
		if (matches(policy.protected_branches, true)) {
			return std::format("'{}' is a protected branch: agents never push to it (vcs.protected_branches)", vcs::valid_utf8(branch));
		}
		if (!matches(policy.push_branches, false)) {
			std::string allowed;
			for (const auto& pattern : policy.push_branches) allowed += (allowed.empty() ? "" : ", ") + vcs::valid_utf8(pattern);
			return std::format("the agent policy does not allow a push to '{}' (vcs.push_branches: {})", vcs::valid_utf8(branch),
				allowed.empty() ? "none" : allowed);
		}
		return std::nullopt;
	};
	return run_job("git push", [args = std::move(args), remote](const git_context& shared, ds::task_context& task) -> json::value {
		git_context git = shared;
		if (git.guarded) {
			if (auto refusal = remote_refusal(git, remote)) return std::move(*refusal);
		}
		if (auto refusal = network_refusal(git, remote, task.stop)) return refusal_json(*refusal, task);
		const git_output out = run_write(git, args, task.stop);
		if (auto failure = write_failure_of(out, task)) return std::move(*failure);
		return output_json(out);
	}, write_mode{ caller.kind, false, true }, rule);
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

	std::vector<std::string> paths_arg(const json::object& args)
	{
		std::vector<std::string> paths;
		if (const json::value* value = args.if_contains("paths")) {
			paths.reserve(value->as_array().size());
			for (const auto& path : value->as_array()) paths.emplace_back(path.as_string());
		}
		return paths;
	}

	constexpr const char* PATHS_ARGS_SCHEMA = R"({
		"type": "object",
		"properties": {
			"paths": { "type": "array", "items": { "type": "string", "minLength": 1 },
				"description": "Relative to the repository root, at least one" }
		},
		"required": ["paths"],
		"additionalProperties": false
	})";

	// A git write: mutates.
	cmd::command_desc write_command(std::string id, cmd::agent_category category, std::string summary, const char* args_schema)
	{
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = json::parse(args_schema);
		desc.result_schema = json::parse(JOB_RESULT_SCHEMA);
		desc.mutates = true;
		desc.category = category;
		return desc;
	}
}

std::vector<edt::registration> edt::register_vcs_commands(cmd::command_registry& commands, vcs_service& service)
{
	std::vector<registration> out;

	cmd::command_desc info;
	info.id = "vcs.info";
	info.category = cmd::agent_category::vcs_read;
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
	status.category = cmd::agent_category::vcs_read;
	status.summary = "Start reading git status (branch, upstream, ahead/behind, changed files); the result arrives in job.get";
	status.result_schema = json::parse(JOB_RESULT_SCHEMA);
	status.invoke = [&service](const cmd::call&) { return job_answer(service.start_status()); };
	keep(out, commands.add(std::move(status)));

	cmd::command_desc diff;
	diff.id = "vcs.diff";
	diff.category = cmd::agent_category::vcs_read;
	diff.summary = "Start reading the working tree diff (or the staged one, or what a commit changed against its first "
		"parent), of one path or all; text capped at 256 KiB; the result arrives in job.get. With commit and path, a file "
		"the commit renamed shows as added (renames are found only among the files of the path)";
	diff.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"path": { "type": "string", "minLength": 1, "description": "Relative to the repository root" },
			"staged": { "type": "boolean" },
			"commit": { "type": "string", "minLength": 1, "description": "A commit hash (4 to 64 hex digits); not with staged" }
		},
		"additionalProperties": false
	})");
	diff.result_schema = json::parse(JOB_RESULT_SCHEMA);
	diff.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		bool staged = false;
		if (const json::value* value = args.if_contains("staged")) staged = value->as_bool();
		if (args.contains("commit")) {
			if (staged) return cmd::result::failure(cmd::codes::invalid_args, "commit and staged cannot be given together");
			return job_answer(service.start_commit_diff(string_arg(args, "commit"), string_arg(args, "path")));
		}
		return job_answer(service.start_diff(string_arg(args, "path"), staged));
	};
	keep(out, commands.add(std::move(diff)));

	cmd::command_desc log;
	log.id = "vcs.log";
	log.category = cmd::agent_category::vcs_read;
	log.summary = "Start reading the commit log of a ref (default HEAD) or of every branch, remote branch and tag (all), "
		"newest first by date, with each commit's refs, optionally of one path or by author or message text (literal, "
		"any case), one page at a time; the result arrives in job.get (error code stale: start again without a cursor)";
	log.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"ref": { "type": "string", "minLength": 1 },
			"all": { "type": "boolean", "description": "Every branch, remote branch and tag; not with ref" },
			"path": { "type": "string", "minLength": 1, "description": "Relative to the repository root" },
			"author": { "type": "string", "description": "Part of the author's name or email, at most 256 bytes" },
			"grep": { "type": "string", "description": "Part of the commit message, at most 256 bytes" },
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
		vcs_service::log_filter filter;
		if (const json::value* value = args.if_contains("all")) filter.all = value->as_bool();
		filter.author = string_arg(args, "author");
		filter.grep = string_arg(args, "grep");
		return job_answer(service.start_log(string_arg(args, "ref"), string_arg(args, "path"), limit, string_arg(args, "cursor"),
			filter));
	};
	keep(out, commands.add(std::move(log)));

	cmd::command_desc show_files;
	show_files.id = "vcs.show_files";
	show_files.category = cmd::agent_category::vcs_read;
	show_files.summary = "Start listing the files a commit changed against its first parent (renames detected); the result "
		"arrives in job.get";
	show_files.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"commit": { "type": "string", "minLength": 1, "description": "A commit hash (4 to 64 hex digits)" }
		},
		"required": ["commit"],
		"additionalProperties": false
	})");
	show_files.result_schema = json::parse(JOB_RESULT_SCHEMA);
	show_files.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_show_files(string_arg(request.args.as_object(), "commit")));
	};
	keep(out, commands.add(std::move(show_files)));

	cmd::command_desc branches;
	branches.id = "vcs.branches";
	branches.category = cmd::agent_category::vcs_read;
	branches.summary = "Start listing the local and remote-tracking branches; the result arrives in job.get";
	branches.result_schema = json::parse(JOB_RESULT_SCHEMA);
	branches.invoke = [&service](const cmd::call&) { return job_answer(service.start_branches()); };
	keep(out, commands.add(std::move(branches)));

	cmd::command_desc stage = write_command("vcs.stage", cmd::agent_category::vcs_commit, "Start staging paths (git add); the result arrives in job.get",
		PATHS_ARGS_SCHEMA);
	stage.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_stage(paths_arg(request.args.as_object()), request.from));
	};
	keep(out, commands.add(std::move(stage)));

	cmd::command_desc unstage = write_command("vcs.unstage", cmd::agent_category::vcs_commit, "Start unstaging paths (git reset -- paths); the work tree "
		"keeps its changes; the result arrives in job.get", PATHS_ARGS_SCHEMA);
	unstage.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_unstage(paths_arg(request.args.as_object()), request.from));
	};
	keep(out, commands.add(std::move(unstage)));

	cmd::command_desc commit = write_command("vcs.commit", cmd::agent_category::vcs_commit, "Start committing what is staged with this message; the "
		"result ({hash}) arrives in job.get", R"({
		"type": "object",
		"properties": { "message": { "type": "string", "minLength": 1 } },
		"required": ["message"],
		"additionalProperties": false
	})");
	commit.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_commit(string_arg(request.args.as_object(), "message"), request.from));
	};
	keep(out, commands.add(std::move(commit)));

	cmd::command_desc branch_create = write_command("vcs.branch.create", cmd::agent_category::vcs_branch, "Start creating a branch at a ref (default HEAD) "
		"without switching to it; the result arrives in job.get", R"({
		"type": "object",
		"properties": {
			"name": { "type": "string", "minLength": 1 },
			"from": { "type": "string", "minLength": 1, "description": "The commit or branch it starts at; default HEAD" }
		},
		"required": ["name"],
		"additionalProperties": false
	})");
	branch_create.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		return job_answer(service.start_branch_create(string_arg(args, "name"), string_arg(args, "from"), request.from));
	};
	keep(out, commands.add(std::move(branch_create)));

	cmd::command_desc checkout = write_command("vcs.checkout", cmd::agent_category::vcs_branch, "Start switching the work tree to a branch or commit "
		"(never forced: local changes it would overwrite stop it); the result arrives in job.get", R"({
		"type": "object",
		"properties": { "ref": { "type": "string", "minLength": 1 } },
		"required": ["ref"],
		"additionalProperties": false
	})");
	checkout.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_checkout(string_arg(request.args.as_object(), "ref"), request.from));
	};
	keep(out, commands.add(std::move(checkout)));

	cmd::command_desc fetch = write_command("vcs.fetch", cmd::agent_category::vcs_sync, "Start fetching from a remote (default: the branch's, else "
		"origin); the result arrives in job.get", R"({
		"type": "object",
		"properties": { "remote": { "type": "string", "minLength": 1, "description": "A configured remote's name" } },
		"additionalProperties": false
	})");
	fetch.invoke = [&service](const cmd::call& request) {
		return job_answer(service.start_fetch(string_arg(request.args.as_object(), "remote"), request.from));
	};
	keep(out, commands.add(std::move(fetch)));

	cmd::command_desc pull = write_command("vcs.pull", cmd::agent_category::vcs_sync, "Start pulling, fast-forward only (no merge commit, no rebase); "
		"the result arrives in job.get", R"({
		"type": "object",
		"properties": {
			"remote": { "type": "string", "minLength": 1, "description": "A configured remote's name" },
			"branch": { "type": "string", "minLength": 1, "description": "The remote's branch; needs remote" }
		},
		"additionalProperties": false
	})");
	pull.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		return job_answer(service.start_pull(string_arg(args, "remote"), string_arg(args, "branch"), request.from));
	};
	keep(out, commands.add(std::move(pull)));

	cmd::command_desc push = write_command("vcs.push", cmd::agent_category::vcs_push, "Start pushing a branch to a remote (never forced: a push that "
		"is not a fast-forward is rejected); the result arrives in job.get", R"({
		"type": "object",
		"properties": {
			"remote": { "type": "string", "minLength": 1, "description": "A configured remote's name" },
			"branch": { "type": "string", "minLength": 1, "description": "A branch name: no leading '+' or '-', no ':'" },
			"set_upstream": { "type": "boolean" }
		},
		"required": ["remote", "branch"],
		"additionalProperties": false
	})");
	push.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		bool set_upstream = false;
		if (const json::value* value = args.if_contains("set_upstream")) set_upstream = value->as_bool();
		return job_answer(service.start_push(string_arg(args, "remote"), string_arg(args, "branch"), set_upstream,
			request.from));
	};
	keep(out, commands.add(std::move(push)));
	return out;
}
