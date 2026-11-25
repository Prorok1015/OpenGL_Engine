#pragma once
#include <snk/editor/edt_problem_list.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// What the text widget marks at one line of the shader file: the rows of the Problems source `shader` that belong to the line.
	struct shader_line_marker
	{
		// 1-based.
		int line = 0;
		// The worst of the rows of the line.
		problem_severity severity = problem_severity::error;
		// One row per line of text: "[code] message (column N)".
		std::string text;
	};

	struct shader_marker_set
	{
		// Ordered by line, one per line.
		std::vector<shader_line_marker> markers;
		// Rows of the file that name no line (a link error, a stage that did not load): shown in the window's banner.
		std::vector<std::string> unplaced;
		// Lines with problems that were left out past k_max_shader_markers.
		std::size_t omitted = 0;
	};

	inline constexpr std::size_t k_max_shader_markers = 200;
	inline constexpr std::size_t k_max_shader_marker_text = 1024;
	inline constexpr std::string_view k_shader_problem_source = "shader";

	// The rows of the source `shader` whose file is `file` (a res:// tag as the renderer prints it), laid on the lines of a text of
	// `line_count` lines. A row past the last line is put on the last one (the text may have been edited since the build); with no
	// lines every row is unplaced.
	shader_marker_set shader_markers_for(const std::vector<sourced_problem>& problems, std::string_view file, int line_count);

	// "[code] message (column N)": the form of one row in a marker and in the banner.
	std::string shader_problem_text(const problem& item);
}
