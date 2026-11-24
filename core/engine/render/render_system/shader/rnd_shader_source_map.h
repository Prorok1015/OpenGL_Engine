#pragma once
#include <snk/model_codec/res_tag.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_shader_problem.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }

// Internal to the renderer: how a line of a prepared shader source is traced back to the file it was written in (#1201).
namespace rnd
{
	// An include that could not be expanded, at the line of the file that has it.
	struct include_problem
	{
		res::tag file;
		int line = 0;
		std::string message;
	};

	struct mapped_includes
	{
		// The files of the source by the number its #line directives give them: 0 is the origin, then each included
		// file in the order it was reached.
		std::vector<res::tag> sources;
		std::vector<include_problem> problems;
	};

	// expand_includes that also wraps every included file in `#line 1 <k>` before its text and `#line <n> <parent>`
	// after it (n: the line after the include in the file that has it), k its index in `sources`: one index per file,
	// however often it is included. Every include is expanded inside the file's guard (`#ifndef SNK_INCLUDE_<hash>`,
	// `#define`, before the `#line 1 <k>`, and `#endif` before the `#line <n> <parent>`), so the compiler, not the
	// expansion, keeps the first copy it takes (#1301). A line the expansion leaves out (an include that failed) keeps its
	// line break, so the lines after it keep their numbers. In every file an #else, #elif or #endif line is followed by
	// `#line <n> <k>` again: a #line in a group the compiler skips is not followed.
	mapped_includes expand_includes_with_lines(std::string& source, const res::tag& origin, res::resource_system& res_sys, std::vector<res::tag>* files);

	struct source_location
	{
		// The index in mapped_includes::sources.
		std::size_t source = 0;
		int line = 0;
	};

	// Where 1-based line `line` of `text` comes from, following the #line directives in it (a text without any: source 0,
	// the same line).
	source_location locate_line(std::string_view text, std::size_t line);

	// The problems a driver's build log tells, at their files: a stage's lines in the NVIDIA (`<k>(<line>) : error ...`),
	// Mesa (`<k>:<line>(<column>): error: ...`) or AMD/ANGLE (`ERROR: <k>:<line>: ...`) form name file k of that stage's
	// `sources` (headers[i] has sources[i]); any other line is a problem of the stage file with no line. Warnings are left
	// out when the log has an error. The link log is one problem of the vertex stage's file (the first stage's without one).
	std::vector<shader_problem> problems_of_build_log(const driver::shader_build_log& log, const std::vector<driver::shader_header>& headers,
		const std::vector<std::vector<res::tag>>& sources, const std::string& program);
}
