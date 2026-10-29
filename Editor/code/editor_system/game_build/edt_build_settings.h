#pragma once
#include <boost/json/fwd.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// How many jobs a build gets: auto = edt::game_builder_options::auto_jobs, manual = the preset's `jobs` decides.
	enum class jobs_mode : std::uint8_t { auto_jobs, manual_jobs };
	// How much of a build's output reaches the log (game_builder::finish).
	enum class log_verbosity : std::uint8_t { errors, normal, verbose };

	// The person's build settings of one project: <root>/.snk/build.json (per user, never committed). Its keys are the
	// field names; jobs_mode is "auto" | "manual", log_verbosity "errors" | "normal" | "verbose".
	struct build_settings
	{
		// "debuggame" or "release"; unset: cfg build/configuration.
		std::optional<std::string> configuration;
		// Qualified: the field names are the JSON keys, the same words as the types.
		edt::jobs_mode jobs_mode = edt::jobs_mode::auto_jobs;
		bool reload_after_build = true;
		// Stored only: there is no Play yet.
		bool build_before_play = false;
		edt::log_verbosity log_verbosity = edt::log_verbosity::normal;
	};

	struct parsed_build_settings
	{
		build_settings settings;
		// One per value that was not understood (its default was kept), or one for text that is not a JSON object.
		std::vector<std::string> warnings;
	};

	inline constexpr std::string_view k_build_settings_file = "build.json";

	std::string_view jobs_mode_name(jobs_mode mode);
	std::string_view log_verbosity_name(log_verbosity verbosity);

	// Unknown keys are ignored.
	parsed_build_settings parse_build_settings(std::string_view json_text);
	// Writes the known keys into `doc` (made an object if it is not one), keeping the other keys and their order.
	void merge_build_settings(boost::json::value& doc, const build_settings& settings);
	// <snk_dir>/build.json; defaults when it is missing (silently) or unreadable (with a warning naming the file).
	build_settings load_build_settings(const std::filesystem::path& snk_dir);
}
