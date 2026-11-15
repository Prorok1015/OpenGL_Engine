#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace res
{
	class resource_system;
}

namespace edt {

	// Whether the person lets the editor load the project's own code (#921): the modules it built into build/bin, and
	// the builds that run its CMake. Kept in the person's own settings, per project by its canonical path
	// (project_settings_key), never in the project, which comes from anyone's repository.
	inline constexpr std::string_view k_project_trust_settings = "project_trust.json";

	enum class trust_answer : std::uint8_t { none, trusted, refused };

	struct project_trust_record
	{
		trust_answer answer = trust_answer::none;
		// The SHA-256 of each module library this editor built for the project, by module name: a library that still
		// hashes so is the person's own build and loads without the question.
		std::map<std::string, std::string> own_builds;
	};

	enum class library_verdict : std::uint8_t { load, refuse, ask };

	// `project`'s record in the text of edt://settings/project_trust.json (nullopt or unreadable: an empty record).
	project_trust_record project_trust_of(const std::optional<std::string>& settings_text, std::string_view project);
	// The settings text with `project`'s answer set (none: forgotten); its own builds and other projects are kept.
	std::string with_trust_answer(const std::optional<std::string>& settings_text, std::string_view project,
		trust_answer answer);
	// The settings text with `sha256` recorded as this editor's build of `module` for `project`.
	std::string with_own_build(const std::optional<std::string>& settings_text, std::string_view project,
		std::string_view module, std::string_view sha256);

	// Whether the library of `module` that hashes to `sha256` (nullopt: it could not be read) may be loaded: the
	// person's answer decides; without one, only the editor's own build of it loads, anything else is asked about.
	library_verdict judge_library(const project_trust_record& record, std::string_view module,
		const std::optional<std::string>& sha256);

	project_trust_record read_project_trust(res::resource_system& res, std::string_view project);
	// The error, nullopt when stored.
	std::optional<std::string> store_trust_answer(res::resource_system& res, std::string_view project,
		trust_answer answer);
	std::optional<std::string> store_own_build(res::resource_system& res, std::string_view project,
		std::string_view module, std::string_view sha256);

}
