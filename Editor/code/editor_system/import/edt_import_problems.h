#pragma once
#include "edt_async_import_task.h"
#include <snk/common/text_escape.h>
#include <snk/editor/edt_problem_list.h>
#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt {

	// The Problems source of the import a person starts (File > Import Model): what the import could not bring in.
	inline constexpr std::string_view k_import_problem_source = "import";

	// Rows of one import in Problems; the textures past them are one more row that counts them.
	inline constexpr std::size_t k_import_problem_rows = 20;

	// One warning per texture the model names that was not loaded (#1108): its material has no sampler for it. The names
	// come from the model file, so they are shown escaped.
	inline std::vector<problem> import_problems(const import_intermediate& imported, std::string_view model_name)
	{
		std::vector<problem> out;
		const std::size_t shown = std::min(imported.missing_textures.size(), k_import_problem_rows);
		out.reserve(shown + 1);
		for (std::size_t i = 0; i < shown; ++i) {
			problem item;
			item.severity = problem_severity::warning;
			item.message = std::format("'{}': texture '{}' was not loaded (missing or not readable): its material has no sampler for it",
				model_name, text_escape::escaped_text(imported.missing_textures[i], 256));
			out.push_back(std::move(item));
		}
		const std::size_t rest = imported.missing_textures.size() - shown + imported.missing_textures_not_listed;
		if (rest > 0) {
			problem item;
			item.severity = problem_severity::warning;
			item.message = std::format("'{}': and {} more texture(s) were not loaded", model_name, rest);
			out.push_back(std::move(item));
		}
		return out;
	}

} // namespace edt
