#include "rnd_shader_source_map.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <regex>
#include <string_view>

namespace
{
	struct log_line
	{
		std::size_t source = 0;
		int line = 0;
		int column = 0;
		std::string message;
	};

	std::string_view trimmed(std::string_view text)
	{
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
			text.remove_prefix(1);
		}
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
			text.remove_suffix(1);
		}
		return text;
	}

	int to_int(const std::ssub_match& match)
	{
		int value = 0;
		const std::string text = match.str();
		std::from_chars(text.data(), text.data() + text.size(), value);
		return value;
	}

	std::optional<log_line> parse_log_line(const std::string& text)
	{
		// NVIDIA: 0(12) : error C0000: ...
		static const std::regex nvidia{ R"(^\s*(\d+)\((\d+)\)\s*:\s*(.*)$)" };
		// Mesa: 0:12(5): error: ...
		static const std::regex mesa{ R"(^\s*(\d+):(\d+)\((\d+)\)\s*:\s*(.*)$)" };
		// AMD, ANGLE: ERROR: 0:12: ...
		static const std::regex amd{ R"(^\s*(ERROR|WARNING)\s*:\s*(\d+):(\d+)\s*:\s*(.*)$)" };
		std::smatch match;
		if (std::regex_match(text, match, mesa)) {
			return log_line{ static_cast<std::size_t>(to_int(match[1])), to_int(match[2]), to_int(match[3]), match[4].str() };
		}
		if (std::regex_match(text, match, nvidia)) {
			return log_line{ static_cast<std::size_t>(to_int(match[1])), to_int(match[2]), 0, match[3].str() };
		}
		if (std::regex_match(text, match, amd)) {
			std::string message = match[4].str();
			if (match[1].str() == "WARNING") {
				message = "warning: " + message;
			}
			return log_line{ static_cast<std::size_t>(to_int(match[2])), to_int(match[3]), 0, std::move(message) };
		}
		return std::nullopt;
	}

	bool is_warning(std::string_view message)
	{
		std::string lower;
		for (const char c : message.substr(0, 7)) {
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return lower == "warning";
	}

	// AMD closes its log with a count of what it already listed.
	bool is_summary(std::string_view text)
	{
		static const std::regex summary{ R"(^\s*ERROR:\s*\d+\s+compilation errors?\..*$)" };
		return std::regex_match(text.begin(), text.end(), summary);
	}

	rnd::shader_problem make_problem(res::tag file, int line, int column, std::string code, std::string message, const std::string& program)
	{
		rnd::shader_problem problem;
		problem.file = std::move(file);
		problem.line = line;
		problem.column = column;
		problem.code = std::move(code);
		problem.message = std::move(message);
		problem.program = program;
		return problem;
	}
}

std::vector<rnd::shader_problem> rnd::problems_of_build_log(const driver::shader_build_log& log, const std::vector<driver::shader_header>& headers,
	const std::vector<std::vector<res::tag>>& sources, const std::string& program)
{
	const auto stage_file = [&](std::size_t stage, std::size_t source) -> res::tag {
		if (stage >= sources.size() || sources[stage].empty()) {
			return {};
		}
		return source < sources[stage].size() ? sources[stage][source] : sources[stage].front();
	};

	std::vector<shader_problem> out;
	std::vector<bool> warnings;
	for (const driver::shader_stage_log& stage : log.stages) {
		const std::size_t before = out.size();
		std::size_t line_begin = 0;
		const std::string_view text = stage.text;
		while (line_begin < text.size()) {
			const std::size_t newline = text.find('\n', line_begin);
			const std::string_view raw = text.substr(line_begin, newline == std::string_view::npos ? std::string_view::npos : newline - line_begin);
			line_begin = newline == std::string_view::npos ? text.size() : newline + 1;
			const std::string_view line = trimmed(raw);
			if (line.empty() || is_summary(line)) {
				continue;
			}
			const std::string owned{ line };
			if (const auto parsed = parse_log_line(owned)) {
				out.push_back(make_problem(stage_file(stage.stage, parsed->source), parsed->line, parsed->column, "shader/compile", parsed->message, program));
				warnings.push_back(is_warning(parsed->message));
			} else {
				out.push_back(make_problem(stage_file(stage.stage, 0), 0, 0, "shader/compile", owned, program));
				warnings.push_back(is_warning(owned));
			}
		}
		if (out.size() == before) {
			out.push_back(make_problem(stage_file(stage.stage, 0), 0, 0, "shader/compile", "the stage did not compile", program));
			warnings.push_back(false);
		}
	}
	if (std::find(warnings.begin(), warnings.end(), false) != warnings.end()) {
		std::vector<shader_problem> errors;
		for (std::size_t i = 0; i < out.size(); ++i) {
			if (!warnings[i]) {
				errors.push_back(std::move(out[i]));
			}
		}
		out = std::move(errors);
	}

	if (!log.link.empty()) {
		std::size_t vertex = 0;
		for (std::size_t i = 0; i < headers.size(); ++i) {
			if (headers[i].type == driver::shader_header::shader_type::VERTEX) {
				vertex = i;
				break;
			}
		}
		const std::string_view text = trimmed(log.link);
		out.push_back(make_problem(stage_file(vertex, 0), 0, 0, "shader/link", text.empty() ? std::string{ "the program did not link" } : std::string{ text }, program));
	}
	return out;
}
