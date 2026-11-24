#pragma once
#include <string>
#include <string_view>

namespace res { class resource_system; }

namespace edt
{
	// The person's modeller settings (#1013): edt://settings/modeller.json, never the project. The steps hold while Ctrl is down during a drag.
	struct modeller_settings
	{
		float move_step = 0.1f;
		float rotate_step_degrees = 15.f;
		float scale_step = 0.1f;

		bool operator==(const modeller_settings&) const = default;
	};

	inline constexpr float k_modeller_min_step = 0.0001f;
	inline constexpr float k_modeller_max_step = 1000.f;
	inline constexpr float k_modeller_max_rotate_step = 180.f;
	inline constexpr std::string_view k_modeller_settings_name = "modeller.json";

	struct modeller_settings_result
	{
		modeller_settings settings;
		// Not empty: the text was not taken and `settings` are the defaults.
		std::string error;
	};
	// {"move_step": 0.1, "rotate_step_degrees": 15, "scale_step": 0.1}; every key optional. A step outside its range is clamped; an
	// unknown key or a value that is not a number is an error.
	modeller_settings_result parse_modeller_settings(std::string_view text);
	std::string serialize_modeller_settings(const modeller_settings& settings);
	modeller_settings clamped(const modeller_settings& settings);

	// A missing file is the defaults without a word; a bad one the defaults with a warning.
	modeller_settings load_modeller_settings(res::resource_system& resources);
	bool save_modeller_settings(res::resource_system& resources, const modeller_settings& settings);
}
