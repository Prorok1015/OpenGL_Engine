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

	// The row of one texture that was not loaded. The importer lists a skipped embedded texture as "*<index>: <why>" (#1206),
	// any other by the name the model gives it.
	inline std::string missing_texture_message(std::string_view model_name, std::string_view listed)
	{
		std::size_t digits = 0;
		while (1 + digits < listed.size() && listed[1 + digits] >= '0' && listed[1 + digits] <= '9') ++digits;
		const bool embedded = listed.starts_with('*') && digits > 0 && listed.substr(1 + digits).starts_with(": ");
		if (embedded) {
			return std::format("'{}': embedded texture {} was skipped: {}: its material has no sampler for it", model_name,
				listed.substr(1, digits), text_escape::escaped_text(listed.substr(digits + 3), 256));
		}
		return std::format("'{}': texture '{}' was not loaded (missing or not readable): its material has no sampler for it",
			model_name, text_escape::escaped_text(listed, 256));
	}

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
			item.message = missing_texture_message(model_name, imported.missing_textures[i]);
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

	// The one row of an import that failed (#1119): it replaces what the last import reported. `model_name` is already
	// escaped; `reason` can quote the model file or the decoder, so it is escaped here.
	inline std::vector<problem> import_failure_problems(std::string_view model_name, std::string_view reason)
	{
		problem item;
		item.severity = problem_severity::error;
		item.message = std::format("'{}': the import failed: {}", model_name, text_escape::escaped_text(reason, 1024));
		std::vector<problem> out;
		out.push_back(std::move(item));
		return out;
	}

} // namespace edt
