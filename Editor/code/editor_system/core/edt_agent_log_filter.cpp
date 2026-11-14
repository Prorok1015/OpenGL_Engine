#include "edt_agent_log_filter.h"
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace {
	namespace fs = std::filesystem;

	constexpr std::array<std::string_view, 5> k_hidden_categories{
		"edt/decoder", "capture", "mcp", "editor/agent_server", "editor/agent_policy",
	};
	constexpr std::string_view k_build_category = "editor/build";

	bool is_or_below(std::string_view category, std::string_view parent)
	{
		return category == parent
			|| (category.size() > parent.size() && category.starts_with(parent) && category[parent.size()] == '/');
	}

	void replace_all(std::string& text, std::string_view what, std::string_view with)
	{
		if (what.size() < 2) return;
		for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size())) {
			text.replace(at, what.size(), with);
		}
	}

	// A root as a log line may spell it: with '/' and, where that differs, as the platform writes it.
	void replace_root(std::string& text, const fs::path& root, std::string_view label)
	{
		if (root.empty()) return;
		const std::string generic = fs_paths::generic_utf8_of(root);
		std::string native = generic;
		std::replace(native.begin(), native.end(), '/', '\\');
		replace_all(text, generic, label);
		if (native != generic) replace_all(text, native, label);
	}

	bool ends_a_path(char c)
	{
		return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '\'' || c == '"' || c == ')' || c == ',' || c == ';' || c == '>';
	}

	bool starts_a_token(const std::string& text, std::size_t at)
	{
		if (at == 0) return true;
		const char before = text[at - 1];
		return std::isspace(static_cast<unsigned char>(before)) != 0 || before == '\'' || before == '"' || before == '(' || before == '=' || before == ':';
	}

	// The length of the absolute path that starts at `at`, 0 when none does.
	std::size_t absolute_path_at(const std::string& text, std::size_t at, const std::vector<std::string>& top_level)
	{
		const auto end_of = [&text](std::size_t from) {
			std::size_t end = from;
			while (end < text.size() && !ends_a_path(text[end])) ++end;
			return end - from;
		};
		const std::string_view rest = std::string_view(text).substr(at);
		// C:\x, C:/x and \\server\share
		if (rest.size() >= 3 && std::isalpha(static_cast<unsigned char>(rest[0])) != 0 && rest[1] == ':' && (rest[2] == '\\' || rest[2] == '/')) {
			return end_of(at);
		}
		if (rest.starts_with("\\\\")) return end_of(at);
		if (!rest.starts_with('/') || rest.starts_with("//")) return 0;
		const std::size_t length = end_of(at);
		const std::string_view path = rest.substr(0, length);
		const std::string_view first = path.substr(1, path.find('/', 1) == std::string_view::npos ? std::string_view::npos : path.find('/', 1) - 1);
		const bool known = std::find(top_level.begin(), top_level.end(), first) != top_level.end();
		return known ? length : 0;
	}
}

edt::agent_log_roots edt::agent_log_roots_of(const fs::path& project)
{
	agent_log_roots roots;
	roots.project = project;
	for (const char* variable : { "HOME", "USERPROFILE" }) {
		if (const char* home = std::getenv(variable); home != nullptr && *home != '\0') {
			roots.home = fs_paths::path_from_utf8(home);
			break;
		}
	}
	std::error_code ec;
	roots.temp = fs::temp_directory_path(ec);
	if (ec) roots.temp.clear();
	for (fs::directory_iterator it("/", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
		roots.top_level.push_back(fs_paths::utf8_of(it->path().filename()));
	}
	return roots;
}

bool edt::agent_log_category_visible(std::string_view category, bool build_allowed)
{
	if (!build_allowed && is_or_below(category, k_build_category)) return false;
	return std::none_of(k_hidden_categories.begin(), k_hidden_categories.end(),
		[category](std::string_view hidden) { return is_or_below(category, hidden); });
}

std::string edt::agent_log_text(std::string_view text, const agent_log_roots& roots)
{
	std::string out(text);
	// The longest root first: a project inside the home folder is the project.
	std::array<std::pair<const fs::path*, std::string_view>, 3> known{ {
		{ &roots.project, "<project>" }, { &roots.temp, "<tmp>" }, { &roots.home, "<home>" } } };
	std::stable_sort(known.begin(), known.end(), [](const auto& a, const auto& b) {
		return a.first->native().size() > b.first->native().size();
	});
	for (const auto& [root, label] : known) {
		replace_root(out, *root, label);
	}
	for (std::size_t at = 0; at < out.size();) {
		const std::size_t length = starts_a_token(out, at) ? absolute_path_at(out, at, roots.top_level) : 0;
		if (length == 0) {
			++at;
			continue;
		}
		constexpr std::string_view k_any = "<path>";
		out.replace(at, length, k_any);
		at += k_any.size();
	}
	return out;
}
