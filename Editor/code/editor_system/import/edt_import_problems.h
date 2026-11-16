#pragma once
#include "edt_async_import_task.h"
#include <snk/common/text_escape.h>
#include <snk/editor/edt_problem_list.h>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt {

	// The Problems source of the import a person starts (File > Import Model): what the import could not bring in.
	inline constexpr std::string_view k_import_problem_source = "import";

	// One warning per texture the model names that was not loaded (#1108): its material has no sampler for it. The names
	// come from the model file, so they are shown escaped.
	inline std::vector<problem> import_problems(const import_intermediate& imported, std::string_view model_name)
	{
		std::vector<problem> out;
		out.reserve(imported.missing_textures.size());
		for (const std::string& texture : imported.missing_textures) {
			problem item;
			item.severity = problem_severity::warning;
			item.message = std::format("'{}': texture '{}' was not loaded (missing or not readable): its material has no sampler for it",
				model_name, text_escape::escaped_text(texture, 256));
			out.push_back(std::move(item));
		}
		return out;
	}

} // namespace edt
