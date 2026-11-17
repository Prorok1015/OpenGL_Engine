#include <snk/render/shader/rnd_shader_bindings.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <string_view>

namespace {
	using namespace rnd::driver;

	struct binding_macro
	{
		std::string_view name;
		binding_type type;
		std::string_view qualifier;
	};

	constexpr binding_macro macros[] = {
		{ "SNK_UBO", binding_type::ubo, "layout(std140, binding = " },
		{ "SNK_SSBO", binding_type::ssbo, "layout(std430, binding = " },
		{ "SNK_TEX", binding_type::texture, "layout(binding = " },
	};

	bool is_identifier_char(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	// A line comment goes on over a newline that is escaped with a backslash, as GLSL says.
	bool newline_is_escaped(const std::string& source, std::size_t newline)
	{
		std::size_t before = newline;
		if (before > 0 && source[before - 1] == '\r') {
			--before;
		}
		return before > 0 && source[before - 1] == '\\';
	}

	// The source with every comment character turned into a space, newlines kept: offsets stay the same, and
	// what is left is the text the compiler would read.
	std::string without_comments(const std::string& source)
	{
		enum class state { code, line, block };
		std::string result = source;
		state current = state::code;
		const auto blank = [&](std::size_t at) {
			if (result[at] != '\n' && result[at] != '\r') {
				result[at] = ' ';
			}
		};
		for (std::size_t i = 0; i < source.size(); ++i) {
			const char next = i + 1 < source.size() ? source[i + 1] : '\0';
			switch (current) {
			case state::code:
				if (source[i] == '/' && next == '/') {
					current = state::line;
					blank(i);
					blank(++i);
				} else if (source[i] == '/' && next == '*') {
					current = state::block;
					blank(i);
					blank(++i);
				}
				break;
			case state::line:
				if (source[i] == '\n') {
					current = newline_is_escaped(source, i) ? state::line : state::code;
				} else {
					blank(i);
				}
				break;
			case state::block:
				if (source[i] == '*' && next == '/') {
					current = state::code;
					blank(i);
					blank(++i);
				} else {
					blank(i);
				}
				break;
			}
		}
		return result;
	}

	std::size_t line_of(const std::string& source, std::size_t at)
	{
		return 1 + static_cast<std::size_t>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
	}

	void skip_spaces(const std::string& source, std::size_t& pos)
	{
		while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t')) {
			++pos;
		}
	}

	bool read_number(const std::string& source, std::size_t& pos, std::uint32_t& value)
	{
		skip_spaces(source, pos);
		if (pos >= source.size() || !std::isdigit(static_cast<unsigned char>(source[pos]))) {
			return false;
		}
		value = 0;
		while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos]))) {
			value = value * 10 + static_cast<std::uint32_t>(source[pos] - '0');
			++pos;
		}
		skip_spaces(source, pos);
		return true;
	}

	bool read_char(const std::string& source, std::size_t& pos, char c)
	{
		skip_spaces(source, pos);
		if (pos >= source.size() || source[pos] != c) {
			return false;
		}
		++pos;
		return true;
	}
}

std::string rnd::expand_binding_macros(std::string& source, const driver::binding_table& table)
{
	// Macros are looked for in the text without comments and copied from the text with them.
	const std::string code = without_comments(source);
	std::string errors;
	std::string result;
	result.reserve(source.size() + 64);

	std::size_t pos = 0;
	while (pos < code.size()) {
		const std::size_t at = code.find("SNK_", pos);
		if (at == std::string::npos) {
			break;
		}
		if (at > 0 && is_identifier_char(code[at - 1])) {
			result.append(source, pos, at + 4 - pos);
			pos = at + 4;
			continue;
		}
		std::size_t name_end = at;
		while (name_end < code.size() && is_identifier_char(code[name_end])) {
			++name_end;
		}
		const std::string_view name{ code.data() + at, name_end - at };

		const binding_macro* macro = nullptr;
		for (const binding_macro& candidate : macros) {
			if (candidate.name == name) {
				macro = &candidate;
			}
		}
		if (!macro) {
			// SNK_VERTEX_SHADER and the like are defines; a call of any other SNK_ name is a misspelt macro.
			std::size_t after = name_end;
			if (read_char(code, after, '(')) {
				errors += std::format("line {}: {} is not a binding macro (SNK_UBO, SNK_SSBO and SNK_TEX are)\n", line_of(source, at), name);
			}
			result.append(source, pos, name_end - pos);
			pos = name_end;
			continue;
		}

		std::size_t cursor = name_end;
		std::uint32_t set = 0;
		std::uint32_t slot = 0;
		const bool parsed = read_char(code, cursor, '(') && read_number(code, cursor, set)
			&& read_char(code, cursor, ',') && read_number(code, cursor, slot) && read_char(code, cursor, ')');
		if (!parsed) {
			errors += std::format("line {}: {} needs (set, slot) as two numbers\n", line_of(source, at), macro->name);
			result.append(source, pos, name_end - pos);
			pos = name_end;
			continue;
		}
		const auto binding = table.resolve(set, slot, macro->type);
		if (!binding) {
			errors += std::format("line {}: {}({}, {}) is outside the binding table\n", line_of(source, at), macro->name, set, slot);
			result.append(source, pos, cursor - pos);
			pos = cursor;
			continue;
		}
		result.append(source, pos, at - pos);
		result += macro->qualifier;
		result += std::to_string(*binding);
		result += ')';
		pos = cursor;
	}
	result.append(source, pos, std::string::npos);
	source = std::move(result);
	return errors;
}

bool rnd::insert_after_version(std::string& source, std::string_view text)
{
	// A UTF-8 byte order mark ahead of the directive is not part of the shader: drop it, compilers refuse it.
	constexpr std::string_view bom = "\xEF\xBB\xBF";
	if (source.starts_with(bom)) {
		source.erase(0, bom.size());
	}
	const std::string code = without_comments(source);
	std::size_t line_begin = 0;
	while (line_begin < code.size()) {
		const std::size_t newline = code.find('\n', line_begin);
		const std::size_t line_end = newline == std::string::npos ? code.size() : newline;
		std::size_t cursor = line_begin;
		while (cursor < line_end && std::isspace(static_cast<unsigned char>(code[cursor]))) {
			++cursor;
		}
		if (cursor < line_end) {
			// The first line with anything on it has to be the directive.
			if (code[cursor] != '#') {
				return false;
			}
			++cursor;
			while (cursor < line_end && (code[cursor] == ' ' || code[cursor] == '\t')) {
				++cursor;
			}
			constexpr std::string_view directive = "version";
			const bool is_version = code.compare(cursor, directive.size(), directive) == 0
				&& (cursor + directive.size() >= line_end || !is_identifier_char(code[cursor + directive.size()]));
			if (!is_version) {
				return false;
			}
			if (newline == std::string::npos) {
				source += '\n';
			}
			source.insert(newline == std::string::npos ? source.size() : newline + 1, text);
			return true;
		}
		line_begin = line_end + 1;
	}
	return false;
}

std::string rnd::blank_comments(const std::string& source)
{
	return without_comments(source);
}
