#include <snk/render/shader/rnd_shader_includes.h>
#include "rnd_shader_source_map.h"
#include <snk/render/shader/rnd_shader_bindings.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
	// Every level of the recursion holds two copies of a file's text; distinct files alone do not bound the depth.
	constexpr std::size_t max_include_depth = 32;
	// A file is expanded at every #include of it (its guard leaves out the copies after the first), so a file included
	// twice by each of a few nested files multiplies: the text is bounded, not only the depth.
	constexpr std::size_t max_expanded_bytes = 4 * 1024 * 1024;

	struct include_context
	{
		res::resource_system& res_sys;
		std::vector<std::string> chain;				// the files being expanded, outermost first
		std::unordered_set<std::string> listed;		// every file already in `files`
		std::unordered_map<std::string, std::size_t> source_index;	// a file's index in `sources`, one per file
		std::size_t expanded_bytes = 0;
		bool too_large_reported = false;
		std::vector<rnd::include_problem> problems;
		std::vector<res::tag>* files = nullptr;
		// Set: every included file is wrapped in #line directives that name its index here (0: the origin).
		std::vector<res::tag>* sources = nullptr;
	};

	// `found` tells the line is an include directive at all; `name` is empty when it is not written as "name".
	struct directive
	{
		bool found = false;
		std::string name;
	};

	directive parse_include(std::string_view code_line)
	{
		std::size_t pos = 0;
		const auto skip_blank = [&] {
			while (pos < code_line.size() && (code_line[pos] == ' ' || code_line[pos] == '\t')) {
				++pos;
			}
		};
		skip_blank();
		if (pos >= code_line.size() || code_line[pos] != '#') {
			return {};
		}
		++pos;
		skip_blank();
		constexpr std::string_view word = "include";
		if (code_line.compare(pos, word.size(), word) != 0) {
			return {};
		}
		pos += word.size();
		if (pos < code_line.size() && (std::isalnum(static_cast<unsigned char>(code_line[pos])) || code_line[pos] == '_')) {
			return {};
		}
		skip_blank();
		directive result{ true, {} };
		if (pos >= code_line.size() || code_line[pos] != '"') {
			return result;
		}
		const std::size_t close = code_line.find('"', pos + 1);
		if (close == std::string_view::npos || close == pos + 1) {
			return result;
		}
		result.name = std::string{ code_line.substr(pos + 1, close - pos - 1) };
		return result;
	}

	struct line_directive
	{
		int line = 0;
		std::optional<std::size_t> source;
	};

	// `#line <line> [<source>]`, as the engine writes it; nullopt for any other line.
	std::optional<line_directive> parse_line_directive(std::string_view code_line)
	{
		std::size_t pos = 0;
		const auto skip_blank = [&] {
			while (pos < code_line.size() && (code_line[pos] == ' ' || code_line[pos] == '\t')) {
				++pos;
			}
		};
		const auto read_number = [&]() -> std::optional<std::size_t> {
			skip_blank();
			const std::size_t begin = pos;
			std::size_t value = 0;
			while (pos < code_line.size() && std::isdigit(static_cast<unsigned char>(code_line[pos]))) {
				value = value * 10 + static_cast<std::size_t>(code_line[pos] - '0');
				++pos;
			}
			return pos == begin ? std::nullopt : std::optional<std::size_t>{ value };
		};
		skip_blank();
		if (pos >= code_line.size() || code_line[pos] != '#') {
			return std::nullopt;
		}
		++pos;
		skip_blank();
		constexpr std::string_view word = "line";
		if (code_line.compare(pos, word.size(), word) != 0) {
			return std::nullopt;
		}
		pos += word.size();
		if (pos >= code_line.size() || (code_line[pos] != ' ' && code_line[pos] != '\t')) {
			return std::nullopt;
		}
		const auto line = read_number();
		if (!line) {
			return std::nullopt;
		}
		line_directive out{ static_cast<int>(*line), read_number() };
		return out;
	}

	// An #else, #elif or #endif line (comments already blanked).
	bool ends_group(std::string_view code_line)
	{
		std::size_t pos = 0;
		const auto skip_blank = [&] {
			while (pos < code_line.size() && (code_line[pos] == ' ' || code_line[pos] == '\t')) {
				++pos;
			}
		};
		skip_blank();
		if (pos >= code_line.size() || code_line[pos] != '#') {
			return false;
		}
		++pos;
		skip_blank();
		for (const std::string_view word : { std::string_view{ "else" }, std::string_view{ "elif" }, std::string_view{ "endif" } }) {
			if (code_line.compare(pos, word.size(), word) == 0) {
				const std::size_t after = pos + word.size();
				if (after >= code_line.size() || !(std::isalnum(static_cast<unsigned char>(code_line[after])) || code_line[after] == '_')) {
					return true;
				}
			}
		}
		return false;
	}

	// The macro of a file's include guard. Hashed: a name made of the path could clash once its characters are replaced,
	// and GLSL reserves names with "__" and the "GL_" prefix.
	std::string guard_macro(std::string_view file)
	{
		std::uint64_t hash = 0xcbf29ce484222325ull;
		for (const char c : file) {
			hash ^= static_cast<unsigned char>(c);
			hash *= 0x100000001b3ull;
		}
		return std::format("SNK_INCLUDE_{:016X}", hash);
	}

	std::string chain_text(const include_context& ctx, const std::string& again)
	{
		std::string text;
		for (const std::string& file : ctx.chain) {
			text += file + " -> ";
		}
		return text + again;
	}

	// The line break `line` ends with ("\r\n", "\n" or none).
	std::string_view line_break_of(std::string_view line)
	{
		if (line.ends_with("\r\n")) {
			return "\r\n";
		}
		return line.ends_with('\n') ? std::string_view{ "\n" } : std::string_view{};
	}

	void expand(std::string& text, const res::tag& file, std::size_t file_index, include_context& ctx)
	{
		const std::string code = rnd::blank_comments(text);
		std::string result;
		result.reserve(text.size());

		std::size_t line_begin = 0;
		std::size_t line_number = 0;
		while (line_begin < text.size()) {
			++line_number;
			const std::size_t newline = text.find('\n', line_begin);
			const std::size_t line_end = newline == std::string::npos ? text.size() : newline + 1;
			const std::string_view line{ text.data() + line_begin, line_end - line_begin };
			const std::string_view code_line{ code.data() + line_begin, line_end - line_begin };
			const directive include = parse_include(code_line);
			line_begin = line_end;

			if (!include.found) {
				result += line;
				// A #line in a group the compiler skips is not followed: numbering is set again where a group ends or turns.
				if (ctx.sources && ends_group(code_line)) {
					if (!line.ends_with('\n')) {
						result += '\n';
					}
					result += std::format("#line {} {}\n", line_number + 1, file_index);
				}
				continue;
			}
			// With #line directives a line left out would shift the lines after it: its line break stays.
			const auto drop_line = [&] {
				if (ctx.sources) {
					result += line_break_of(line);
				}
			};
			const auto report = [&](std::string message) {
				ctx.problems.push_back({ file, static_cast<int>(line_number), std::move(message) });
				drop_line();
			};
			if (include.name.empty()) {
				report("#include needs a file name in double quotes");
				continue;
			}
			const res::tag target = file + res::tag::make(include.name);
			const std::string key = target.string();
			if (std::ranges::find(ctx.chain, key) != ctx.chain.end()) {
				report(std::format("include cycle {}", chain_text(ctx, key)));
				continue;
			}
			if (ctx.chain.size() >= max_include_depth) {
				report(std::format("include depth exceeded ({} files deep) {}", max_include_depth, chain_text(ctx, key)));
				continue;
			}
			if (ctx.files && ctx.listed.insert(key).second) {
				ctx.files->push_back(target);
			}
			const auto included = ctx.res_sys.require_sync<res::text_resource>(target).get_sync();
			if (!included) {
				report(std::format("cannot read include \"{}\" ({})", include.name, key));
				continue;
			}
			// Checked after the read so one huge file counts as well as a file included again and again.
			if (ctx.expanded_bytes + included->str().size() > max_expanded_bytes) {
				if (ctx.too_large_reported) {
					drop_line();
				} else {
					ctx.too_large_reported = true;
					report(std::format("the includes expand to more than {} bytes: a file too large, or included again and again", max_expanded_bytes));
				}
				continue;
			}
			std::string body = included->str();
			// Expanded at every #include: the guard, not the expansion, leaves out a copy the compiler already has. An
			// include in a group the compiler skips does not hide the file from a later include in a group it keeps.
			const std::string guard = guard_macro(key);
			const std::string_view eol = line_break_of(line).empty() ? std::string_view{ "\n" } : line_break_of(line);
			const std::string guard_open = std::format("#ifndef {0}{1}#define {0}{1}", guard, eol);
			ctx.expanded_bytes += body.size() + guard_open.size();
			result += guard_open;
			std::size_t target_index = 0;
			if (ctx.sources) {
				const auto [index, added] = ctx.source_index.try_emplace(key, ctx.sources->size());
				if (added) {
					ctx.sources->push_back(target);
				}
				target_index = index->second;
				result += std::format("#line 1 {}\n", target_index);
			}
			ctx.chain.push_back(key);
			expand(body, target, target_index, ctx);
			ctx.chain.pop_back();
			result += body;
			if (!body.empty() && body.back() != '\n') {
				result += '\n';
			}
			result += "#endif";
			result += eol;
			if (ctx.sources) {
				result += std::format("#line {} {}\n", line_number + 1, file_index);
			}
		}
		text = std::move(result);
	}
}

std::string rnd::expand_includes(std::string& source, const res::tag& origin, res::resource_system& res_sys, std::vector<res::tag>* files)
{
	include_context ctx{ res_sys, {}, {}, {}, 0, false, {}, files, nullptr };
	ctx.chain.push_back(origin.string());
	ctx.listed.insert(origin.string());
	expand(source, origin, 0, ctx);
	std::string errors;
	for (const include_problem& problem : ctx.problems) {
		errors += std::format("'{}' line {}: {}\n", problem.file.view(), problem.line, problem.message);
	}
	return errors;
}

rnd::mapped_includes rnd::expand_includes_with_lines(std::string& source, const res::tag& origin, res::resource_system& res_sys, std::vector<res::tag>* files)
{
	mapped_includes out;
	out.sources.push_back(origin);
	include_context ctx{ res_sys, {}, {}, {}, 0, false, {}, files, &out.sources };
	ctx.chain.push_back(origin.string());
	ctx.listed.insert(origin.string());
	ctx.source_index.emplace(origin.string(), 0);
	expand(source, origin, 0, ctx);
	out.problems = std::move(ctx.problems);
	return out;
}

rnd::source_location rnd::locate_line(std::string_view text, std::size_t line)
{
	// The line a directive gives is the one of the line after it.
	source_location at{ 0, 1 };
	std::size_t current = 1;
	std::size_t line_begin = 0;
	while (current < line && line_begin < text.size()) {
		const std::size_t newline = text.find('\n', line_begin);
		const std::size_t line_end = newline == std::string_view::npos ? text.size() : newline;
		if (const auto directive = parse_line_directive(text.substr(line_begin, line_end - line_begin))) {
			at.line = directive->line;
			if (directive->source) {
				at.source = *directive->source;
			}
		} else {
			++at.line;
		}
		++current;
		if (newline == std::string_view::npos) {
			break;
		}
		line_begin = newline + 1;
	}
	return at;
}
