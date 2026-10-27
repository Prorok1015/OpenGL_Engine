#pragma once
#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <snk/common/snk_api.h>

namespace prj
{
	struct engine_version
	{
		int major = 0;
		int minor = 0;
		int patch = 0;

		auto operator<=>(const engine_version&) const = default;
	};

	// How a project's engine_version relates to the running editor. Projects are compatible within one MAJOR.MINOR
	// (before 1.0 a minor bump is a breaking one, so the rule is the same on both sides of 1.0); the patch is free.
	enum class version_match
	{
		exact,
		patch_differs,
		project_older,
		project_newer,
	};

	// The version of the engine this binary was built from (SNK_SDK_VERSION in cmake/SnkInstall.cmake).
	SNK_API engine_version current_engine_version();

	// Strict "MAJOR.MINOR.PATCH" of non-negative decimal numbers; nullopt for anything else.
	SNK_API std::optional<engine_version> parse_engine_version(std::string_view text);
	SNK_API std::string to_string(const engine_version& version);

	SNK_API version_match compare_engine_versions(const engine_version& project, const engine_version& editor);

	// What the editor does with a project of that match. `mismatch_allowed`: the user already agreed to open a
	// project of another MAJOR.MINOR (--allow-engine-version-mismatch).
	enum class version_action
	{
		open,
		open_with_notice,
		// The project is older: open it without migrating, or quit.
		ask_open_or_quit,
		// The project is newer than the editor: it cannot be opened.
		refuse,
	};

	SNK_API version_action version_action_for(version_match match, bool mismatch_allowed);
}
