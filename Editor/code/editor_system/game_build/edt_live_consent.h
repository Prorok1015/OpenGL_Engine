#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace res
{
	class resource_system;
}

namespace edt {

	struct build_settings;

	// Live mode builds and loads the project's code with no question asked, so turning it on is the person's consent
	// (#908). It is kept in the person's own settings, per project by its canonical path, never in the project:
	// .snk/build.json comes with the project, from anyone's repository.
	inline constexpr std::string_view k_live_mode_settings = "live_mode.json";

	// Whether the person turned live mode on for `project` (the text of edt://settings/live_mode.json; nullopt: none).
	bool live_consented(const std::optional<std::string>& settings_text, std::string_view project);

	// The settings text with `project` turned on or off; other projects and unknown keys are kept.
	std::string with_live_consent(const std::optional<std::string>& settings_text, std::string_view project,
		bool enabled);

	// The consent of `project` as edt://settings/live_mode.json of `res` holds it now.
	bool read_live_consent(res::resource_system& res, std::string_view project);
	// Turns `project` on or off in edt://settings/live_mode.json of `res`; the error, nullopt when it is stored.
	std::optional<std::string> store_live_consent(res::resource_system& res, std::string_view project, bool enabled);

	// Live mode at the opening of `project`: on only by the person's consent. A live_enabled of build.json is ignored,
	// with a warning when it asks for live mode the person did not turn on.
	bool live_mode_at_open(const build_settings& project_settings, const std::optional<std::string>& settings_text,
		std::string_view project);

}
