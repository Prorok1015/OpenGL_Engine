#include <snk/render/shader/rnd_shader_includes.h>
#include <snk/render/shader/rnd_shader_bindings.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
	struct include_context
	{
		res::resource_system& res_sys;
		std::vector<std::string> chain;				// the files being expanded, outermost first
		std::unordered_set<std::string> expanded;	// every file that already stands in the output
		std::string errors;
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

	std::string chain_text(const include_context& ctx, const std::string& again)
	{
		std::string text;
		for (const std::string& file : ctx.chain) {
			text += file + " -> ";
		}
		return text + again;
	}

	void expand(std::string& text, const res::tag& file, include_context& ctx)
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
			const directive include = parse_include(std::string_view{ code.data() + line_begin, line_end - line_begin });
			line_begin = line_end;

			if (!include.found) {
				result += line;
				continue;
			}
			const std::string where = std::format("'{}' line {}", file.view(), line_number);
			if (include.name.empty()) {
				ctx.errors += std::format("{}: #include needs a file name in double quotes\n", where);
				continue;
			}
			const res::tag target = file + res::tag::make(include.name);
			const std::string key = target.string();
			if (std::ranges::find(ctx.chain, key) != ctx.chain.end()) {
				ctx.errors += std::format("{}: include cycle {}\n", where, chain_text(ctx, key));
				continue;
			}
			if (!ctx.expanded.insert(key).second) {
				continue;
			}
			const auto included = ctx.res_sys.require_sync<res::text_resource>(target).get_sync();
			if (!included) {
				ctx.errors += std::format("{}: cannot read include \"{}\" ({})\n", where, include.name, key);
				continue;
			}
			std::string body = included->str();
			ctx.chain.push_back(key);
			expand(body, target, ctx);
			ctx.chain.pop_back();
			result += body;
			if (!body.empty() && body.back() != '\n') {
				result += '\n';
			}
		}
		text = std::move(result);
	}
}

std::string rnd::expand_includes(std::string& source, const res::tag& origin, res::resource_system& res_sys)
{
	include_context ctx{ res_sys, {}, {}, {} };
	ctx.chain.push_back(origin.string());
	ctx.expanded.insert(origin.string());
	expand(source, origin, ctx);
	return ctx.errors;
}
