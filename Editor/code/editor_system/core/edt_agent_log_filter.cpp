#include "edt_agent_log_filter.h"
#include "edt_agent_policy.h"
#include "edt_environment.h"
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <system_error>
#include <utility>

namespace {
	namespace fs = std::filesystem;

	constexpr std::array<std::string_view, 5> k_hidden_categories{
		"edt/decoder", "capture", "mcp", "editor/agent_server", "editor/agent_policy",
	};
	constexpr std::string_view k_project_label = "<project>";
	constexpr std::string_view k_home_label = "<home>";
	constexpr std::string_view k_temp_label = "<tmp>";
	constexpr std::string_view k_any_label = "<path>";
	constexpr std::array<std::string_view, 4> k_labels{ k_project_label, k_home_label, k_temp_label, k_any_label };
#ifdef _WIN32
	constexpr std::array<const char*, 2> k_home_variables{ "USERPROFILE", "HOME" };
#else
	constexpr std::array<const char*, 1> k_home_variables{ "HOME" };
#endif

	bool is_or_below(std::string_view category, std::string_view parent)
	{
		return category == parent
			|| (category.size() > parent.size() && category.starts_with(parent) && category[parent.size()] == '/');
	}

	bool is_space(char c)
	{
		return std::isspace(static_cast<unsigned char>(c)) != 0;
	}

	bool is_letter(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	bool is_separator(char c)
	{
		return c == '/' || c == '\\';
	}

	// What a path is made of; a path starts after anything else. The bytes of a UTF-8 letter count as letters.
	bool is_path_char(char c)
	{
		const auto byte = static_cast<unsigned char>(c);
		return byte >= 0x80 || std::isalnum(byte) != 0 || c == '_' || c == '.' || c == '-' || is_separator(c);
	}

	bool ends_a_path(char c)
	{
		return is_space(c) || c == '\'' || c == '"' || c == ')' || c == ',' || c == ';' || c == '>' || c == ']' || c == '}' || c == '|';
	}

	std::size_t token_end(std::string_view text, std::size_t from)
	{
		std::size_t end = from;
		while (end < text.size() && !ends_a_path(text[end])) ++end;
		return end;
	}

	bool same_letter(char a, char b)
	{
#ifdef _WIN32
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
#else
		return a == b;
#endif
	}

	bool starts_a_token(std::string_view text, std::size_t at)
	{
		if (at == 0) return true;
		const char before = text[at - 1];
		if (is_path_char(before)) return false;
		if (before != '>') return true;
		// What follows a stand-in is the rest of a path that is already written.
		const std::string_view head = text.substr(0, at);
		return std::none_of(k_labels.begin(), k_labels.end(), [head](std::string_view label) { return head.ends_with(label); });
	}

	struct known_root
	{
		// With '/' and without a trailing one.
		std::string text;
		std::string_view label;
	};

	std::vector<known_root> known_roots(const edt::agent_log_roots& roots)
	{
		std::vector<known_root> out;
		const auto add = [&out](const std::vector<fs::path>& folders, std::string_view label) {
			for (const fs::path& folder : folders) {
				// "/" or "C:\" alone would be the root of every path.
				if (folder.relative_path().empty()) continue;
				std::string text = fs_paths::generic_utf8_of(folder);
				while (text.size() > 1 && text.back() == '/') text.pop_back();
				out.push_back(known_root{ std::move(text), label });
			}
		};
		add(roots.project, k_project_label);
		add(roots.temp, k_temp_label);
		add(roots.home, k_home_label);
		// The longest root first: a project inside the home folder is the project.
		std::stable_sort(out.begin(), out.end(), [](const known_root& a, const known_root& b) { return a.text.size() > b.text.size(); });
		return out;
	}

	// Whether `root` is at `from` as a whole folder name: "/p/demo" is not the root of "/p/demo2".
	bool root_matches(std::string_view text, std::size_t from, std::string_view root)
	{
		if (text.size() - from < root.size()) return false;
		for (std::size_t i = 0; i < root.size(); ++i) {
			const char have = text[from + i];
			if (root[i] == '/' ? !is_separator(have) : !same_letter(have, root[i])) return false;
		}
		const std::size_t after = from + root.size();
		if (after == text.size() || is_separator(text[after]) || !is_path_char(text[after])) return true;
		// The full stop of a sentence, not "demo.bak".
		return text[after] == '.' && (after + 1 == text.size() || !is_path_char(text[after + 1]));
	}

	bool is_drive(std::string_view rest)
	{
		return rest.size() >= 3 && is_letter(rest[0]) && rest[1] == ':' && is_separator(rest[2]);
	}

	// \\server\share and //server/share
	bool is_share(std::string_view rest)
	{
		if (rest.size() < 5 || !is_separator(rest[0]) || rest[1] != rest[0]) return false;
		std::size_t name_end = 2;
		while (name_end < rest.size() && is_path_char(rest[name_end]) && !is_separator(rest[name_end])) ++name_end;
		return name_end > 2 && name_end + 1 < rest.size() && is_separator(rest[name_end])
			&& is_path_char(rest[name_end + 1]) && !is_separator(rest[name_end + 1]);
	}

	// The length of what is glued in front of a path: "-I" of "-I/usr/x", "/Fo" of "/FoC:\x", "\\?\" of "\\?\C:\x".
	std::size_t prefix_length(std::string_view rest)
	{
		if (rest.starts_with("\\\\?\\") || rest.starts_with("\\\\.\\")) return 4;
		if (rest.empty() || (rest[0] != '-' && rest[0] != '/')) return 0;
		std::size_t letters = 1;
		while (letters < rest.size() && is_letter(rest[letters])) ++letters;
		if (letters == 1 || letters == rest.size()) return 0;
		if (is_drive(rest.substr(letters - 1))) return letters - 1;
		return rest[0] == '-' && rest[letters] == '/' ? letters : 0;
	}

	// Whether the word before `at` is a resource tag or a desc file: what follows one is a JSON pointer into it.
	bool follows_a_resource(std::string_view text, std::size_t at)
	{
		const auto between = [](char c) { return is_space(c) || c == ':' || c == ',' || c == '(' || c == '\'' || c == '"'; };
		std::size_t end = at;
		while (end > 0 && between(text[end - 1])) --end;
		std::size_t begin = end;
		while (begin > 0 && !is_space(text[begin - 1])) --begin;
		const std::string_view word = text.substr(begin, end - begin);
		return (word.find("://") != std::string_view::npos && !word.starts_with("file://")) || word.ends_with(".desc");
	}

	// Whether `path` ("/a/b/c") has two segments at least and the first two ("/a/b") are on the disk.
	bool first_two_exist(std::string_view path, const edt::agent_log_roots& roots)
	{
		const std::size_t second = path.find_first_of("/\\", 1);
		if (second == std::string_view::npos || second == 1) return false;
		std::size_t end = path.find_first_of("/\\", second + 1);
		if (end == std::string_view::npos) end = path.size();
		if (end == second + 1) return false;
		try {
			const fs::path folder = fs_paths::path_from_utf8(path.substr(0, end));
			if (roots.on_disk) return roots.on_disk(folder);
			std::error_code ec;
			return fs::exists(folder, ec);
		} catch (const std::exception&) {
			// Bytes that are no text in the platform's encoding name no file.
			return false;
		}
	}

	// Where a path with a space in a folder name goes on to ("C:\Program Files\x"); 0 when the space ends it.
	std::size_t end_after_space(std::string_view text, std::size_t end)
	{
		if (end + 1 >= text.size() || text[end] != ' ') return 0;
		const std::size_t next_end = token_end(text, end + 1);
		const std::string_view part = text.substr(end + 1, next_end - end - 1);
		if (part.empty() || part.find_first_of("/\\") == std::string_view::npos) return 0;
		// A path of its own, a flag or an address is not the rest of this one.
		if (is_separator(part[0]) || part[0] == '-' || part[0] == '<' || is_drive(part) || part.find("://") != std::string_view::npos) return 0;
		return next_end;
	}

	struct found_path
	{
		// What the stand-in replaces; nothing when `length` is 0.
		std::size_t from = 0;
		std::size_t length = 0;
		std::string_view label;
		// What is left as it is after the stand-in (or from the place looked at, when there is none).
		std::size_t skip = 1;
	};

	found_path path_at(std::string_view text, std::size_t at, const std::vector<known_root>& known, const edt::agent_log_roots& roots)
	{
		std::size_t from = at;
		bool in_file_address = false;
		if (at > 0 && text[at - 1] == ':' && text.substr(at).starts_with("//")) {
			// Only file:// names a file; what follows another scheme is a host or a resource and stays.
			if (!text.substr(0, at).ends_with("file:")) return found_path{ at, 0, {}, token_end(text, at) - at };
			in_file_address = true;
			from = at + 2;
			if (from >= text.size() || ends_a_path(text[from])) return {};
			if (text[from] != '/') return found_path{ at, token_end(text, at) - at, k_any_label, 0 };
			if (is_drive(text.substr(from + 1))) ++from;
		} else {
			from = at + prefix_length(text.substr(at));
		}

		const std::string_view rest = text.substr(from);
		const bool drive = is_drive(rest);
		const bool share = from == at && is_share(rest);
		const bool rooted = rest.size() >= 2 && rest[0] == '/' && !is_separator(rest[1]);
		if (!drive && !share && !rooted) return {};

		for (const known_root& root : known) {
			if (!root_matches(text, from, root.text)) continue;
			const std::size_t after = from + root.text.size();
			return found_path{ from, root.text.size(), root.label, token_end(text, after) - after };
		}

		const auto without_punctuation = [text, from](std::size_t last) {
			while (last > from + 1 && (text[last - 1] == '.' || text[last - 1] == ':')) --last;
			return last;
		};
		std::size_t end = token_end(text, from);
		std::size_t trimmed = without_punctuation(end);
		// "/header/type" may as well be a JSON pointer: a path only when it leads somewhere on this disk.
		if (rooted && !in_file_address) {
			if (follows_a_resource(text, at) || !first_two_exist(text.substr(from, trimmed - from), roots)) return {};
		}
		while (trimmed == end) {
			const std::size_t more = end_after_space(text, end);
			if (more == 0) break;
			end = more;
			trimmed = without_punctuation(end);
		}
		return found_path{ from, trimmed - from, k_any_label, 0 };
	}

	void add_root(std::vector<fs::path>& out, const fs::path& folder)
	{
		if (folder.empty()) return;
		std::error_code ec;
		fs::path spelled = fs::absolute(folder, ec).lexically_normal();
		if (ec || spelled.empty()) return;
		if (!spelled.has_filename()) spelled = spelled.parent_path();
		out.push_back(spelled);
		// As the system names it: /private/tmp for /tmp on macOS, the target of a linked project folder.
		fs::path resolved = fs::weakly_canonical(spelled, ec);
		if (!ec && !resolved.empty() && resolved != spelled) out.push_back(std::move(resolved));
	}
}

edt::agent_log_roots edt::agent_log_roots_of(const fs::path& project)
{
	agent_log_roots roots;
	add_root(roots.project, project);
	for (const char* variable : k_home_variables) {
		if (const auto home = environment_utf8(variable); home && !home->empty()) {
			add_root(roots.home, fs_paths::path_from_utf8(*home));
			break;
		}
	}
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	if (!ec) add_root(roots.temp, temp);
	return roots;
}

bool edt::agent_log_category_visible(std::string_view category, bool build_allowed)
{
	if (!build_allowed && is_or_below(category, k_agent_build_category)) return false;
	return std::none_of(k_hidden_categories.begin(), k_hidden_categories.end(),
		[category](std::string_view hidden) { return is_or_below(category, hidden); });
}

std::string edt::agent_log_text(std::string_view text, const agent_log_roots& roots)
{
	const std::vector<known_root> known = known_roots(roots);
	std::string out(text);
	for (std::size_t at = 0; at < out.size();) {
		const found_path found = starts_a_token(out, at) ? path_at(out, at, known, roots) : found_path{};
		if (found.length == 0) {
			at += found.skip;
			continue;
		}
		// The label is a literal: it stays valid while `out` changes under it.
		out.replace(found.from, found.length, found.label);
		at = found.from + found.label.size() + found.skip;
	}
	return out;
}

bool edt::agent_build_output_allowed(agent_policy& policy, const cmd::command_registry& commands)
{
	const cmd::command_desc* build = commands.find("build.game_module");
	return build != nullptr && policy.effective(*build).mode == cmd::policy_mode::allow;
}

void edt::install_agent_log_filter(cmd::log_read_filter& filter, std::function<bool()> build_allowed, agent_log_roots roots)
{
	// Asked for every entry of a read: the policy (which looks at its files) is asked a few times a second at most.
	struct build_answer
	{
		bool asked = false;
		bool allowed = false;
		std::chrono::steady_clock::time_point at{};
	};
	filter.visible = [build_allowed = std::move(build_allowed), answer = std::make_shared<build_answer>()](std::string_view category, const cmd::origin& from) {
		if (from.kind == cmd::origin_kind::ui) return true;
		const auto now = std::chrono::steady_clock::now();
		if (!answer->asked || now - answer->at > std::chrono::milliseconds(250)) {
			answer->asked = true;
			answer->allowed = build_allowed && build_allowed();
			answer->at = now;
		}
		return agent_log_category_visible(category, answer->allowed);
	};
	filter.redact = [roots = std::move(roots)](std::string_view text, const cmd::origin& from) {
		return from.kind == cmd::origin_kind::ui ? std::string(text) : agent_log_text(text, roots);
	};
}
