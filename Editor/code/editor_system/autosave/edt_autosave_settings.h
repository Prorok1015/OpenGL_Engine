#pragma once
#include <string>
#include <string_view>

namespace res { class resource_system; }

namespace edt
{
	// The person's autosave settings (#1186): edt://settings/autosave.json, never the project.
	struct autosave_settings
	{
		bool enabled = true;
		// From the first unsaved edit to the write.
		int interval_seconds = 60;

		bool operator==(const autosave_settings&) const = default;
	};

	inline constexpr int k_autosave_min_interval = 5;
	inline constexpr int k_autosave_max_interval = 24 * 60 * 60;
	inline constexpr std::string_view k_autosave_settings_name = "autosave.json";

	struct autosave_settings_result
	{
		autosave_settings settings;
		// Not empty: the text was not taken and `settings` are the defaults.
		std::string error;
	};
	// {"enabled": true, "interval_seconds": 60}; both keys optional. An interval outside 5..86400 is clamped; an unknown key
	// or a wrong type is an error.
	autosave_settings_result parse_autosave_settings(std::string_view text);
	std::string serialize_autosave_settings(const autosave_settings& settings);

	// A missing file is the defaults without a word; a bad one the defaults with a warning.
	autosave_settings load_autosave_settings(res::resource_system& resources);
	bool save_autosave_settings(res::resource_system& resources, const autosave_settings& settings);

	// cfg `editor/autosave` (default true): false turns autosave off whatever the person chose (a test run, CI).
	bool autosave_allowed_by_config();
}
