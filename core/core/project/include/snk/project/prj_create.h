#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <snk/common/snk_api.h>

namespace prj
{
	struct create_result
	{
		// The project.snkproj of the new project; empty exactly when `error` is set.
		std::filesystem::path manifest_file;
		std::string error;
	};

	// File > New Project (EPIC-42 #158): copies the template folder `template_dir` (a project.snkproj and its content)
	// to `target_dir` and writes `name` and the running engine's version into the copied manifest. `target_dir` must
	// not exist or be an empty folder; a failed creation leaves nothing behind.
	SNK_API create_result create_project(const std::filesystem::path& template_dir, const std::filesystem::path& target_dir,
		std::string_view name);
}
