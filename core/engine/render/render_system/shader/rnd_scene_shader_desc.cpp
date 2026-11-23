#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/render/rnd_gpu_blocks.h>
#include <snk/render/shader/rnd_shader_bindings.h>
#include <snk/render/shader/rnd_shader_includes.h>
#include "rnd_shader_source_map.h"
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <cctype>
#include <charconv>
#include <format>
#include <sstream>

// The engine's own defines: the one source of the light array length (the std140 layout of GlobalLights depends
// on it, see lights_params) and the stage the source is compiled for.
static std::string engine_defines(rnd::driver::shader_header::shader_type type)
{
    const char* stage = "SNK_VERTEX_SHADER";
    if (type == rnd::driver::shader_header::shader_type::FRAGMENT) stage = "SNK_FRAGMENT_SHADER";
    if (type == rnd::driver::shader_header::shader_type::GEOMETRY) stage = "SNK_GEOMETRY_SHADER";
    return std::format("#define MAX_LIGHT_COUNT {}\n#define {}\n", rnd::lights_params::MAX_LIGHT_COUNT, stage);
}

static rnd::shader_problem make_problem(res::tag file, int line, std::string code, std::string message)
{
	rnd::shader_problem problem;
	problem.file = std::move(file);
	problem.line = line;
	problem.code = std::move(code);
	problem.message = std::move(message);
	return problem;
}

// The 1-based line of the first thing in `body` that is not a comment or blank.
static std::size_t first_code_line(const std::string& body)
{
	const std::string code = rnd::blank_comments(body);
	std::size_t line = 1;
	for (const char c : code) {
		if (c == '\n') {
			++line;
		} else if (!std::isspace(static_cast<unsigned char>(c))) {
			break;
		}
	}
	return line;
}

// The lines "line N: message" expand_binding_macros returns, as problems of the files the lines come from.
static void add_macro_problems(std::string_view errors, const std::string& body, const std::vector<res::tag>& sources, std::vector<rnd::shader_problem>& out)
{
	std::size_t line_begin = 0;
	while (line_begin < errors.size()) {
		const std::size_t newline = errors.find('\n', line_begin);
		const std::string_view text = errors.substr(line_begin, newline == std::string_view::npos ? std::string_view::npos : newline - line_begin);
		line_begin = newline == std::string_view::npos ? errors.size() : newline + 1;
		if (text.empty()) {
			continue;
		}
		rnd::shader_problem problem;
		problem.file = sources.front();
		problem.code = "shader/macro";
		problem.message = std::string{ text };
		constexpr std::string_view prefix = "line ";
		const std::size_t colon = text.find(": ");
		std::size_t line = 0;
		if (text.starts_with(prefix) && colon != std::string_view::npos) {
			const std::string_view number = text.substr(prefix.size(), colon - prefix.size());
			std::from_chars(number.data(), number.data() + number.size(), line);
		}
		if (line > 0) {
			const rnd::source_location at = rnd::locate_line(body, line);
			if (at.source < sources.size()) {
				problem.file = sources[at.source];
			}
			problem.line = at.line;
			problem.message = std::string{ text.substr(colon + 2) };
		}
		out.push_back(std::move(problem));
	}
}

// Fills result.error, result.problems and result.sources: what is wrong with the stages (nothing when they are fit
// for the compiler) and the files each stage's #line directives number.
static void preprocess_shader_code(const std::vector<res::tag>& tags, std::vector<rnd::driver::shader_header>& headers, const rnd::shader_config::constant_data& cdata, res::resource_system& res_sys, const rnd::driver::binding_table& bindings, rnd::shader_config::load_result& result)
{
	ASSERT_MSG(tags.size() == headers.size(), "tags and headers sizes should be equal");

	std::string& problems = result.error;
	result.sources.reserve(headers.size());
	for (std::size_t index = 0; index < headers.size(); ++index)
	{
		auto& header = headers[index];
		rnd::mapped_includes expanded = rnd::expand_includes_with_lines(header.body, tags[index], res_sys, &result.files);
		for (rnd::include_problem& problem : expanded.problems) {
			problems += std::format("'{}': '{}' line {}: {}\n", header.title, problem.file.view(), problem.line, problem.message);
			result.problems.push_back(make_problem(std::move(problem.file), problem.line, "shader/include", std::move(problem.message)));
		}
		result.sources.push_back(std::move(expanded.sources));
	}

	std::vector<std::string> dbg_addition_defines_names;

	std::ostringstream addition_defines;
	for (const auto& [name, value] : cdata.constants) {
		auto define = std::vformat("#define {0} {1}\n", std::make_format_args(name, value));
		addition_defines << define;
		dbg_addition_defines_names.push_back(name);
	}

	for (const auto& def : cdata.defines) {
		auto define = std::vformat("#define {0}\n", std::make_format_args(def));
		addition_defines << define;
		dbg_addition_defines_names.push_back(def);
	}

	std::string defines_title;
	if (!dbg_addition_defines_names.empty()) {
		std::string format = dbg_addition_defines_names.front();
		for (std::size_t i = 1; i < dbg_addition_defines_names.size(); ++i) {
			auto& name_ = dbg_addition_defines_names[i];
			format = std::vformat("{0} | {1}", std::make_format_args(format, name_));
		}
		defines_title = std::vformat("{{ {0} }}", std::make_format_args(format));

		for (auto& header : headers) {
			header.title.insert(header.title.find_first_of('.'), defines_title);
		}
	}

	if (!defines_title.empty()) {
		result.program += " " + defines_title;
	}

	for (std::size_t index = 0; index < headers.size(); ++index) {
		auto& header = headers[index];
		// Lines count from the top of the source after the #include lines were replaced by the files.
		const std::string errors = rnd::expand_binding_macros(header.body, bindings);
		if (!errors.empty()) {
			problems += std::format("'{}': {}", header.title, errors);
			add_macro_problems(errors, header.body, result.sources[index], result.problems);
		}
		// The compiler numbers the lines after the inserted defines as the file does again (source 0: the stage).
		const rnd::source_location after_version = rnd::locate_line(header.body, first_code_line(header.body));
		const std::string inserted = engine_defines(header.type) + std::string{ addition_defines.view() }
			+ std::format("#line {} {}\n", after_version.line + 1, after_version.source);
		if (!rnd::insert_after_version(header.body, inserted)) {
			problems += std::format("'{}': the source has to begin with a #version directive\n", header.title);
			result.problems.push_back(make_problem(tags[index], 0, "shader/load", "the source has to begin with a #version directive"));
		}
	}
	for (rnd::shader_problem& problem : result.problems) {
		problem.program = result.program;
	}
}

rnd::shader_config::load_result rnd::shader_config::load(res::resource_system& res_sys, const driver::binding_table& bindings) const
{
	return load(cdata, res_sys, bindings);
}

rnd::shader_config::load_result rnd::shader_config::load(const constant_data& cdata, res::resource_system& res_sys, const driver::binding_table& bindings)
{
    std::vector<res::tag> tags;
    std::vector<res::tag> unreadable;	// stages that could not be read: still files of the source, one may appear later
    std::vector<driver::shader_header> headers;
	headers.reserve(3);
    tags.reserve(3);

	auto culc_shader_type = [](const std::string_view ext)
		{
			if (ext == "vert") return driver::shader_header::shader_type::VERTEX;
			if (ext == "frag") return driver::shader_header::shader_type::FRAGMENT;
			if (ext == "geom") return driver::shader_header::shader_type::GEOMETRY;
			return driver::shader_header::shader_type::VERTEX;
		};

	for (const auto& tag : cdata.program)
	{
		if (!tag.is_valid()) continue;

		auto shaderCode = res_sys.require_sync<res::text_resource>(tag).get_sync();
		if (shaderCode) {
            headers.push_back(driver::shader_header{ 
                .title = std::string{tag.name()}, 
                .body = shaderCode->c_str(), 
                .type = culc_shader_type(tag.extension())
                });
			tags.push_back(tag);
		} else {
			unreadable.push_back(tag);
		}
	}

    load_result result;
    for (const auto& tag : cdata.program) {
        if (tag.is_valid()) {
            result.program += result.program.empty() ? std::string{ tag.name() } : " + " + std::string{ tag.name() };
        }
    }
    result.files = tags;
    result.files.insert(result.files.end(), unreadable.begin(), unreadable.end());
    preprocess_shader_code(tags, headers, cdata, res_sys, bindings, result);
    if (result.error.empty() && headers.empty()) {
        result.error = "no stage of the program could be read\n";
        for (const res::tag& tag : unreadable) {
            result.problems.push_back(make_problem(tag, 0, "shader/load", "the stage cannot be read"));
        }
        if (result.problems.empty()) {
            result.problems.push_back(make_problem(res::tag{}, 0, "shader/load", "the program has no stage"));
        }
        for (shader_problem& problem : result.problems) {
            problem.program = result.program;
        }
    }
    if (!result.error.empty()) {
        egLOG_ERROR("shader/load", "shader not loaded:\n{}", result.error);
        return result;
    }
    result.headers = std::move(headers);
    return result;
}
