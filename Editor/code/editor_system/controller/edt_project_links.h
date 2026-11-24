#pragma once
#include <snk/editor/edt_problem_list.h>
#include <snk/project/prj_project.h>
#include <string_view>
#include <vector>

namespace edt
{
	// The source of the problems about the project's own folders.
	inline constexpr std::string_view k_project_problem_source = "project";

	// Links in code/ that lead outside the project, as warnings (#1300). The project still opens: the build (CMake) and a person's
	// reads follow such a link, as the project's author meant; the agent's code.* tools never follow one (they refuse a link
	// at or below the path they are given), and nothing writes through a link. A walk that could not finish says so.
	[[nodiscard]] std::vector<problem> code_link_problems(const prj::project_context& project);
}
