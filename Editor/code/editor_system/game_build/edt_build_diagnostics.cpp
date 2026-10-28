#include "edt_build_diagnostics.h"
#include <algorithm>
#include <array>
#include <utility>

namespace {
	using edt::problem;
	using edt::problem_severity;
	using sv = std::string_view;

	bool is_digit(char c) { return c >= '0' && c <= '9'; }
	bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
	bool is_alnum(char c) { return is_digit(c) || is_alpha(c); }

	sv trim(sv s)
	{
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
		return s;
	}

	// A decimal number at s[i], advancing i. More than 9 digits is not a line number.
	bool read_number(sv s, std::size_t& i, int& out)
	{
		const std::size_t start = i;
		int value = 0;
		while (i < s.size() && is_digit(s[i]) && i - start < 9) {
			value = value * 10 + (s[i] - '0');
			++i;
		}
		if (i == start || (i < s.size() && is_digit(s[i]))) return false;
		out = value;
		return true;
	}

	struct kind_match
	{
		problem_severity severity = problem_severity::error;
		sv rest;
	};

	std::optional<kind_match> leading_kind(sv s)
	{
		static constexpr std::array<std::pair<sv, problem_severity>, 4> words = { {
			{ "fatal error", problem_severity::error },
			{ "error", problem_severity::error },
			{ "warning", problem_severity::warning },
			{ "note", problem_severity::note } } };
		for (const auto& [word, severity] : words) {
			if (s.starts_with(word)) return kind_match{ severity, s.substr(word.size()) };
		}
		return std::nullopt;
	}

	// "error: message" (GCC, Clang, ld, CMake-less tools).
	std::optional<kind_match> gcc_kind(sv s)
	{
		auto kind = leading_kind(s);
		if (!kind || !kind->rest.starts_with(':')) return std::nullopt;
		kind->rest = trim(kind->rest.substr(1));
		return kind;
	}

	bool is_gcc_context(sv rest)
	{
		return rest.starts_with("required from") || rest.starts_with("required by");
	}

	struct msvc_match
	{
		problem_severity severity = problem_severity::error;
		sv code;
		sv message;
	};

	// "error C2065: message", "fatal error LNK1104: ...", "command line warning D9002: ...", "note: ...",
	// "error: ..." (clang-cl has no code).
	std::optional<msvc_match> msvc_kind(sv s, bool need_code)
	{
		if (s.starts_with("command line ")) s.remove_prefix(13);
		msvc_match out;
		sv rest;
		if (auto kind = leading_kind(s)) {
			out.severity = kind->severity;
			rest = kind->rest;
		} else if (s.starts_with("message")) {
			out.severity = problem_severity::note;
			rest = s.substr(7);
		} else {
			return std::nullopt;
		}
		std::size_t i = 0;
		while (i < rest.size() && rest[i] == ' ') ++i;
		const std::size_t code_start = i;
		while (i < rest.size() && is_alnum(rest[i])) ++i;
		out.code = rest.substr(code_start, i - code_start);
		// "errors: ..." is not "error s:".
		if (code_start == 0 && !out.code.empty()) return std::nullopt;
		while (i < rest.size() && rest[i] == ' ') ++i;
		if (i >= rest.size() || rest[i] != ':') return std::nullopt;
		if (need_code && out.code.empty()) return std::nullopt;
		out.message = trim(rest.substr(i + 1));
		return out;
	}

	struct located
	{
		sv file;
		int line = 0;
		int column = 0;
		sv rest;
	};

	// "path(12): ...", "path(12,5): ...", "path(12) : ..." (older cl). Tries every '(' since a path may hold some.
	std::optional<located> msvc_location(sv s)
	{
		for (std::size_t open = s.find('('); open != sv::npos; open = s.find('(', open + 1)) {
			if (open == 0) continue;
			std::size_t i = open + 1;
			int line = 0;
			int column = 0;
			if (!read_number(s, i, line)) continue;
			if (i < s.size() && s[i] == ',') {
				++i;
				if (!read_number(s, i, column)) continue;
			}
			const sv tail = s.substr(i);
			if (tail.starts_with("):")) return located{ s.substr(0, open), line, column, trim(tail.substr(2)) };
			if (tail.starts_with(") :")) return located{ s.substr(0, open), line, column, trim(tail.substr(3)) };
		}
		return std::nullopt;
	}

	// "path:12:5: rest" or "path:12: rest" where accept(rest). A drive letter ("C:/", "C:\") is part of the path.
	template<class Accept>
	std::optional<located> colon_location(sv s, Accept accept)
	{
		const bool drive = s.size() >= 3 && is_alpha(s[0]) && s[1] == ':' && (s[2] == '/' || s[2] == '\\');
		for (std::size_t colon = s.find(':', drive ? 2 : 0); colon != sv::npos; colon = s.find(':', colon + 1)) {
			if (colon == 0) continue;
			std::size_t i = colon + 1;
			int line = 0;
			if (!read_number(s, i, line) || i >= s.size() || s[i] != ':') continue;
			++i;
			int column = 0;
			std::size_t j = i;
			if (read_number(s, j, column) && j < s.size() && s[j] == ':') {
				i = j + 1;
			} else {
				column = 0;
			}
			const located at{ s.substr(0, colon), line, column, trim(s.substr(i)) };
			if (accept(at.rest)) return at;
		}
		return std::nullopt;
	}

	sv base_name(sv path)
	{
		const std::size_t slash = path.find_last_of("/\\");
		if (slash != sv::npos) path.remove_prefix(slash + 1);
		if (path.ends_with(".exe")) path.remove_suffix(4);
		return path;
	}

	// ld, ld.bfd, ld.gold, ld.lld, ld64.lld, x86_64-linux-gnu-ld, ...
	bool is_linker(sv tool)
	{
		const sv base = base_name(tool);
		return base == "ld" || base.starts_with("ld.") || base.starts_with("ld64") || base.ends_with("-ld")
			|| base.find("-ld.") != sv::npos;
	}

	bool equals_ignore_case(sv a, sv b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
			return lower(x) == lower(y);
		});
	}

	// The tool, not a file: "LINK : fatal error LNK1104", "cl : command line warning D9002".
	bool is_msvc_tool(sv name)
	{
		for (const sv tool : { sv("link"), sv("cl"), sv("lib"), sv("mt"), sv("rc"), sv("cvtres") }) {
			if (equals_ignore_case(name, tool)) return true;
		}
		return false;
	}

	// Drops ANSI CSI sequences (ESC '[' parameters, final byte 0x40-0x7E): coloured GCC/Clang output.
	void strip_escapes(sv line, std::string& out)
	{
		out.clear();
		out.reserve(line.size());
		for (std::size_t i = 0; i < line.size(); ++i) {
			if (line[i] != '\x1b' || i + 1 >= line.size() || line[i + 1] != '[') {
				out += line[i];
				continue;
			}
			std::size_t end = i + 2;
			while (end < line.size() && !(line[end] >= 0x40 && line[end] <= 0x7E)) ++end;
			i = end;
		}
	}

	// "[3/10] Building CXX object ..." (ninja).
	bool is_progress(sv line)
	{
		if (!line.starts_with('[')) return false;
		std::size_t i = 1;
		int ignored = 0;
		if (!read_number(line, i, ignored) || i >= line.size() || line[i] != '/') return false;
		++i;
		return read_number(line, i, ignored) && i < line.size() && line[i] == ']';
	}

	problem make(problem_severity severity, sv file, int line, int column, sv code, sv message)
	{
		problem out;
		out.severity = severity;
		out.file = std::string(file);
		out.line = line;
		out.column = column;
		out.code = std::string(code);
		out.message = std::string(message);
		return out;
	}

	// "obj:(.text+0x1a): message" -> {obj, message}.
	std::optional<std::pair<sv, sv>> section_reference(sv s)
	{
		const std::size_t open = s.find(":(");
		if (open == sv::npos || open == 0) return std::nullopt;
		const std::size_t close = s.find("):", open);
		if (close == sv::npos) return std::nullopt;
		return std::pair{ s.substr(0, open), trim(s.substr(close + 2)) };
	}
}

void edt::build_output_parser::feed(std::string_view line)
{
	if (line.find('\x1b') != sv::npos) {
		strip_escapes(line, m_clean);
		line = m_clean;
	}
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
	if (continue_cmake(line)) return;
	if (line.empty() || line.front() == ' ' || line.front() == '\t') return;
	if (line.starts_with("FAILED: ") || line.starts_with("ninja: build stopped") || line.starts_with("-- ") || is_progress(line)) return;
	if (line.starts_with(">>> ")) {
		add_note(line);
		return;
	}
	if (parse_cmake(line)) return;

	if (const auto at = msvc_location(line)) {
		if (const auto kind = msvc_kind(at->rest, false)) {
			if (kind->severity == problem_severity::note) {
				add_note(line);
			} else {
				add(make(kind->severity, at->file, at->line, at->column, kind->code, kind->message));
			}
			return;
		}
	}

	const auto gcc_accept = [](sv rest) { return gcc_kind(rest).has_value() || is_gcc_context(rest); };
	if (const auto at = colon_location(line, gcc_accept)) {
		if (is_gcc_context(at->rest)) {
			m_context.emplace_back(line);
			return;
		}
		const auto kind = gcc_kind(at->rest);
		if (kind->severity == problem_severity::note) {
			add_note(line);
		} else {
			add(make(kind->severity, at->file, at->line, at->column, {}, kind->rest));
		}
		return;
	}
	if (line.find(": In instantiation of ") != sv::npos) {
		m_context.emplace_back(line);
		return;
	}

	const std::size_t tool_end = line.find(": ");
	const sv tool = tool_end == sv::npos ? sv{} : line.substr(0, tool_end);
	// A space may be in the linker's directory ("C:/Program Files/.../ld.exe"), not in its name.
	if (!tool.empty() && base_name(tool).find(' ') == sv::npos && is_linker(tool)) {
		const sv rest = trim(line.substr(tool_end + 1));
		if (const auto kind = gcc_kind(rest)) {
			if (kind->severity == problem_severity::note) {
				add_note(line);
			} else {
				add(make(kind->severity, {}, 0, 0, {}, kind->rest));
			}
			return;
		}
		// Context ("a.o: in function `f()':") and chatter, not problems.
		if (rest.find(": in function ") != sv::npos || rest.starts_with("skipping incompatible")) return;
		if (const auto reference = section_reference(rest)) {
			add(make(problem_severity::error, reference->first, 0, 0, {}, reference->second));
			return;
		}
		if (const auto at = colon_location(rest, [](sv message) { return !message.empty(); })) {
			add(make(problem_severity::error, at->file, at->line, 0, {}, at->rest));
			return;
		}
		add(make(problem_severity::error, {}, 0, 0, {}, rest));
		return;
	}
	// Older GCC drivers print the linker's errors without its name.
	if (const auto reference = section_reference(line)) {
		if (reference->second.find("undefined reference") != sv::npos || reference->second.find("multiple definition") != sv::npos) {
			add(make(problem_severity::error, reference->first, 0, 0, {}, reference->second));
			return;
		}
	}

	if (const std::size_t separator = line.find(" : "); separator != sv::npos && separator > 0) {
		if (const auto kind = msvc_kind(trim(line.substr(separator + 3)), true)) {
			const sv thing = line.substr(0, separator);
			if (kind->severity == problem_severity::note) {
				add_note(line);
			} else {
				add(make(kind->severity, is_msvc_tool(thing) ? sv{} : thing, 0, 0, kind->code, kind->message));
			}
			return;
		}
	}

	// "collect2: error: ...", "clang: error: ...", "cc1plus: warning: ...".
	if (!tool.empty() && tool.find(' ') == sv::npos) {
		if (const auto kind = gcc_kind(trim(line.substr(tool_end + 1)))) {
			if (kind->severity == problem_severity::note) {
				add_note(line);
			} else {
				add(make(kind->severity, {}, 0, 0, {}, kind->rest));
			}
		}
	}
}

std::vector<edt::problem> edt::build_output_parser::take()
{
	std::vector<problem> out = std::move(m_items);
	m_items.clear();
	m_context.clear();
	m_cmake_open.reset();
	return out;
}

void edt::build_output_parser::add(problem item)
{
	if (!m_context.empty()) {
		item.notes = std::move(m_context);
		m_context.clear();
	}
	m_items.push_back(std::move(item));
}

void edt::build_output_parser::add_note(std::string_view line)
{
	if (m_items.empty()) return;
	m_items.back().notes.emplace_back(trim(line));
}

bool edt::build_output_parser::continue_cmake(std::string_view line)
{
	if (!m_cmake_open) return false;
	std::string& message = m_items[*m_cmake_open].message;
	const sv text = trim(line);
	if (text.empty()) {
		// The text may start after a blank line. find_package says what to do in the second paragraph, so the
		// message ends at the second blank line after the text started.
		if (!message.empty() && ++m_cmake_breaks >= 2) m_cmake_open.reset();
		return true;
	}
	if (line.starts_with("  ")) {
		if (!message.empty()) message += ' ';
		message += text;
		return true;
	}
	m_cmake_open.reset();
	return false;
}

// "CMake Error at CMakeLists.txt:12 (find_package):", "CMake Warning (dev) at ...", "CMake Error in CMakeLists.txt:",
// "CMake Error: message". The text of the first three follows on indented lines.
bool edt::build_output_parser::parse_cmake(std::string_view line)
{
	if (!line.starts_with("CMake ")) return false;
	const sv head = line.substr(6);
	const std::size_t at = head.find(" at ");
	const std::size_t in = head.find(" in ");
	const std::size_t colon = head.find(':');
	const std::size_t cut = std::min({ at, in, colon });
	if (cut == sv::npos) return false;

	const sv kind = head.substr(0, cut);
	problem item;
	if (kind.find("Error") != sv::npos) {
		item.severity = problem_severity::error;
	} else if (kind.find("Warning") != sv::npos) {
		item.severity = problem_severity::warning;
	} else {
		return false;
	}

	if (cut == colon) {
		item.message = std::string(trim(head.substr(colon + 1)));
	} else {
		sv where = trim(head.substr(cut + 4));
		if (where.ends_with(':')) where.remove_suffix(1);
		if (where.ends_with(')')) {
			if (const std::size_t paren = where.rfind(" ("); paren != sv::npos) where = where.substr(0, paren);
		}
		item.file = std::string(where);
		if (cut == at) {
			const std::size_t separator = where.rfind(':');
			std::size_t i = separator + 1;
			int number = 0;
			if (separator != sv::npos && separator > 0 && read_number(where, i, number) && i == where.size()) {
				item.file = std::string(where.substr(0, separator));
				item.line = number;
			}
		}
	}
	const bool text_follows = item.message.empty();
	add(std::move(item));
	if (text_follows) {
		m_cmake_open = m_items.size() - 1;
		m_cmake_breaks = 0;
	}
	return true;
}
