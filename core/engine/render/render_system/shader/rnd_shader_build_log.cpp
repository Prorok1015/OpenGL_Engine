#include "rnd_shader_source_map.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
	// A driver's log is the driver's: a line, the whole log and the rows it gives are bounded before anything reads them.
	constexpr std::size_t max_line_bytes = 4 * 1024;
	constexpr std::size_t max_log_bytes = 256 * 1024;
	constexpr std::size_t max_problems = 100;
	constexpr std::string_view ellipsis = "\xE2\x80\xA6";

	struct log_line
	{
		std::size_t source = 0;
		int line = 0;
		int column = 0;
		std::string_view message;
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

	// At most `limit` bytes of `text`, never ending inside a UTF-8 sequence.
	std::string_view utf8_prefix(std::string_view text, std::size_t limit)
	{
		if (text.size() <= limit) {
			return text;
		}
		std::size_t end = limit;
		while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
			--end;
		}
		return text.substr(0, end);
	}

	// `text` within max_line_bytes, not cut inside a UTF-8 sequence, with an ellipsis when it was cut here or before (`cut`).
	std::string bounded(std::string_view text, bool cut)
	{
		const std::string_view kept = utf8_prefix(text, max_line_bytes);
		std::string out{ kept };
		cut = cut || kept.size() < text.size();
		if (cut) {
			out += ellipsis;
		}
		return out;
	}

	struct cursor
	{
		std::string_view text;
		std::size_t pos = 0;

		void skip_blank()
		{
			while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
				++pos;
			}
		}

		bool eat(char c)
		{
			if (pos < text.size() && text[pos] == c) {
				++pos;
				return true;
			}
			return false;
		}

		bool eat(std::string_view word)
		{
			if (text.substr(pos).starts_with(word)) {
				pos += word.size();
				return true;
			}
			return false;
		}

		// A run of digits; leading zeros aside, more than 9 of them is no number a log means, so none at all.
		std::optional<std::size_t> number()
		{
			const std::size_t begin = pos;
			while (pos < text.size() && text[pos] == '0') {
				++pos;
			}
			std::size_t value = 0;
			std::size_t significant = 0;
			while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
				value = value * 10 + static_cast<std::size_t>(text[pos] - '0');
				if (++significant > 9) {
					return std::nullopt;
				}
				++pos;
			}
			return pos == begin ? std::nullopt : std::optional<std::size_t>{ value };
		}

		// `:` with blanks around it, then the rest of the line.
		std::optional<std::string_view> rest_after_colon()
		{
			skip_blank();
			if (!eat(':')) {
				return std::nullopt;
			}
			skip_blank();
			return text.substr(pos);
		}
	};

	// NVIDIA `k(line) : ...` or Mesa `k:line(column): ...`.
	std::optional<log_line> parse_numbered(std::string_view text)
	{
		cursor at{ text };
		const auto source = at.number();
		if (!source) {
			return std::nullopt;
		}
		log_line out;
		out.source = *source;
		if (at.eat('(')) {
			const auto line = at.number();
			if (!line || !at.eat(')')) {
				return std::nullopt;
			}
			out.line = static_cast<int>(*line);
		} else if (at.eat(':')) {
			const auto line = at.number();
			if (!line || !at.eat('(')) {
				return std::nullopt;
			}
			const auto column = at.number();
			if (!column || !at.eat(')')) {
				return std::nullopt;
			}
			out.line = static_cast<int>(*line);
			out.column = static_cast<int>(*column);
		} else {
			return std::nullopt;
		}
		const auto message = at.rest_after_colon();
		if (!message) {
			return std::nullopt;
		}
		out.message = *message;
		return out;
	}

	// AMD, ANGLE: `ERROR: k:line: ...` (or WARNING).
	std::optional<log_line> parse_tagged(std::string_view text, bool& warning)
	{
		cursor at{ text };
		warning = at.eat("WARNING");
		if (!warning && !at.eat("ERROR")) {
			return std::nullopt;
		}
		at.skip_blank();
		if (!at.eat(':')) {
			return std::nullopt;
		}
		at.skip_blank();
		const auto source = at.number();
		if (!source || !at.eat(':')) {
			return std::nullopt;
		}
		const auto line = at.number();
		if (!line) {
			return std::nullopt;
		}
		const auto message = at.rest_after_colon();
		if (!message) {
			return std::nullopt;
		}
		return log_line{ *source, static_cast<int>(*line), 0, *message };
	}

	// AMD closes its log with a count of what it already listed: `ERROR: N compilation errors. ...`.
	bool is_summary(std::string_view text)
	{
		cursor at{ text };
		if (!at.eat("ERROR")) {
			return false;
		}
		at.skip_blank();
		if (!at.eat(':')) {
			return false;
		}
		at.skip_blank();
		if (!at.number()) {
			return false;
		}
		at.skip_blank();
		return at.eat("compilation error");
	}

	bool starts_with_warning(std::string_view message)
	{
		constexpr std::string_view word = "warning";
		if (message.size() < word.size()) {
			return false;
		}
		for (std::size_t i = 0; i < word.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(message[i])) != word[i]) {
				return false;
			}
		}
		return true;
	}

	rnd::shader_problem make_problem(res::tag file, int line, int column, std::string message, const std::string& program, std::string code = "shader/compile")
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

	bool is_version_line(std::string_view line)
	{
		cursor at{ line };
		at.skip_blank();
		if (!at.eat('#')) {
			return false;
		}
		at.skip_blank();
		return at.eat("version") && (at.pos == line.size() || line[at.pos] == ' ' || line[at.pos] == '\t');
	}

	// The lines of the block of defines shader_config::load put after the #version line: `#line 1 <index>` opens it,
	// the next #line (back to the stage) closes it. Empty when `body` has no such block.
	std::vector<std::string_view> injected_lines(std::string_view body, std::size_t index)
	{
		const std::string opening = std::format("#line 1 {}", index);
		std::vector<std::string_view> lines;
		bool after_version = false;
		bool inside = false;
		std::size_t line_begin = 0;
		while (line_begin < body.size()) {
			const std::size_t newline = body.find('\n', line_begin);
			std::string_view line = body.substr(line_begin, newline == std::string_view::npos ? std::string_view::npos : newline - line_begin);
			line_begin = newline == std::string_view::npos ? body.size() : newline + 1;
			if (line.ends_with('\r')) {
				line.remove_suffix(1);
			}
			if (inside) {
				if (line.starts_with("#line")) {
					return lines;
				}
				lines.push_back(line);
			} else if (after_version && line == opening) {
				inside = true;
			} else {
				after_version = is_version_line(line);
			}
		}
		return inside ? lines : std::vector<std::string_view>{};
	}

	// "injected define #k "<its line>" (engine|constant|define): " for line `k` of the block, as the log numbers it.
	std::string injected_prefix(const std::vector<std::string_view>& block, int k, std::size_t constant_count)
	{
		if (k < 1 || static_cast<std::size_t>(k) > block.size()) {
			return std::format("injected define #{}: ", k);
		}
		const std::size_t at = static_cast<std::size_t>(k);
		const std::string_view kind = at <= rnd::engine_define_lines ? "engine"
			: at <= rnd::engine_define_lines + constant_count ? "constant" : "define";
		return std::format("injected define #{} \"{}\" ({}): ", k, bounded(block[at - 1], false), kind);
	}

	// The rows of one kind (errors or warnings): the first max_problems kept, the rest counted.
	struct bounded_rows
	{
		std::vector<rnd::shader_problem> kept;
		std::size_t total = 0;

		void add(rnd::shader_problem problem)
		{
			++total;
			if (kept.size() < max_problems) {
				kept.push_back(std::move(problem));
			}
		}
	};
}

std::vector<rnd::shader_problem> rnd::problems_of_build_log(const driver::shader_build_log& log, const std::vector<driver::shader_header>& headers,
	const std::vector<std::vector<res::tag>>& sources, const std::string& program, std::size_t constant_count)
{
	const auto stage_file = [&](std::size_t stage) -> res::tag {
		return stage < sources.size() && !sources[stage].empty() ? sources[stage].front() : res::tag{};
	};

	bounded_rows errors;
	bounded_rows warnings;
	std::size_t budget = max_log_bytes;
	for (const driver::shader_stage_log& stage : log.stages) {
		const std::size_t before = errors.total + warnings.total;
		const bool mapped = stage.stage < sources.size() && stage.stage < headers.size();
		const std::vector<std::string_view> block = mapped ? injected_lines(headers[stage.stage].body, sources[stage.stage].size()) : std::vector<std::string_view>{};
		const std::string_view text = utf8_prefix(stage.text, budget);
		budget -= text.size();
		std::size_t line_begin = 0;
		while (line_begin < text.size()) {
			const std::size_t newline = text.find('\n', line_begin);
			const std::size_t line_end = newline == std::string_view::npos ? text.size() : newline;
			const bool cut = line_end - line_begin > max_line_bytes;
			const std::string_view line = trimmed(utf8_prefix(text.substr(line_begin, line_end - line_begin), max_line_bytes));
			line_begin = line_end + 1;
			if (line.empty() || is_summary(line)) {
				continue;
			}
			bool tagged_warning = false;
			std::optional<log_line> parsed = parse_numbered(line);
			if (!parsed) {
				parsed = parse_tagged(line, tagged_warning);
			}
			if (!parsed) {
				std::string message = bounded(line, cut);
				const bool warning = starts_with_warning(message);
				(warning ? warnings : errors).add(make_problem(stage_file(stage.stage), 0, 0, std::move(message), program));
				continue;
			}
			std::string message = bounded(parsed->message, cut);
			const bool warning = tagged_warning || starts_with_warning(message);
			if (tagged_warning) {
				message = "warning: " + message;
			}
			// A source number the stage's table does not have names no file of it, so its line is no line of a file either.
			const bool known = stage.stage < sources.size() && parsed->source < sources[stage.stage].size();
			const bool injected = !known && !block.empty() && parsed->source == sources[stage.stage].size();
			if (injected) {
				message = injected_prefix(block, parsed->line, constant_count) + message;
			}
			auto problem = known
				? make_problem(sources[stage.stage][parsed->source], parsed->line, parsed->column, std::move(message), program)
				: make_problem(stage_file(stage.stage), 0, 0, std::move(message), program);
			(warning ? warnings : errors).add(std::move(problem));
		}
		if (errors.total + warnings.total == before) {
			errors.add(make_problem(stage_file(stage.stage), 0, 0, "the stage did not compile", program));
		}
	}
	// Warnings only matter when nothing else says why the program did not build.
	bounded_rows& chosen = errors.total > 0 ? errors : warnings;
	std::vector<shader_problem> out = std::move(chosen.kept);
	std::size_t more = chosen.total - out.size();

	if (!log.link.empty()) {
		std::size_t vertex = 0;
		for (std::size_t i = 0; i < headers.size(); ++i) {
			if (headers[i].type == driver::shader_header::shader_type::VERTEX) {
				vertex = i;
				break;
			}
		}
		const std::string_view link = utf8_prefix(log.link, budget);
		const std::string_view text = trimmed(utf8_prefix(link, max_line_bytes));
		if (out.size() < max_problems) {
			out.push_back(make_problem(stage_file(vertex), 0, 0,
				text.empty() ? std::string{ "the program did not link" } : bounded(text, link.size() > max_line_bytes), program, "shader/link"));
		} else {
			++more;
		}
	}
	if (more > 0) {
		out.push_back(make_problem(stage_file(0), 0, 0, std::format("{} {} more problems", ellipsis, more), program));
	}
	return out;
}
