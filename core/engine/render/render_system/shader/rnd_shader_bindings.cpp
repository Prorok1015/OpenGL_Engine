#include <snk/render/shader/rnd_shader_bindings.h>

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

	bool in_line_comment(const std::string& source, std::size_t at)
	{
		const std::size_t line_start = source.rfind('\n', at);
		const std::size_t from = line_start == std::string::npos ? 0 : line_start + 1;
		return source.find("//", from) < at;
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
	std::string errors;
	std::string result;
	result.reserve(source.size() + 64);

	std::size_t pos = 0;
	while (pos < source.size()) {
		const std::size_t at = source.find("SNK_", pos);
		if (at == std::string::npos) {
			break;
		}
		const binding_macro* macro = nullptr;
		for (const binding_macro& candidate : macros) {
			const bool before = at == 0 || !is_identifier_char(source[at - 1]);
			if (before && source.compare(at, candidate.name.size(), candidate.name) == 0
				&& (at + candidate.name.size() >= source.size() || !is_identifier_char(source[at + candidate.name.size()]))) {
				macro = &candidate;
				break;
			}
		}
		if (!macro || in_line_comment(source, at)) {
			result.append(source, pos, at + 4 - pos);
			pos = at + 4;
			continue;
		}

		std::size_t cursor = at + macro->name.size();
		std::uint32_t set = 0;
		std::uint32_t slot = 0;
		const bool parsed = read_char(source, cursor, '(') && read_number(source, cursor, set)
			&& read_char(source, cursor, ',') && read_number(source, cursor, slot) && read_char(source, cursor, ')');
		if (!parsed) {
			errors += std::format("{} needs (set, slot) as two numbers\n", macro->name);
			result.append(source, pos, at + macro->name.size() - pos);
			pos = at + macro->name.size();
			continue;
		}
		const auto binding = table.resolve(set, slot, macro->type);
		if (!binding) {
			errors += std::format("{}({}, {}) is outside the binding table\n", macro->name, set, slot);
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
