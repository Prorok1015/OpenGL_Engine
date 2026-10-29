#pragma once
#include <snk/project/prj_json_text.h>
#include <boost/json/fwd.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
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

	inline constexpr int k_default_live_quiet_ms = 3000;
	inline constexpr int k_max_live_quiet_ms = 600000;
	inline constexpr int k_max_live_jobs = 1024;

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
		// Configurations whose next configure runs with --fresh: a user define was removed, and CMakeCache.txt of the
		// shared build tree still holds it. Cleared by the builder after a successful configure.
		std::vector<std::string> fresh_configure;
		// Live mode (#445): rebuild in the background after the sources stay unchanged for live_quiet_ms.
		bool live_enabled = false;
		// [0, k_max_live_quiet_ms].
		int live_quiet_ms = k_default_live_quiet_ms;
		// [0, k_max_live_jobs]; 0: auto (make_live_build_request).
		int live_jobs = 0;

		bool operator==(const build_settings&) const = default;
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

	// Unknown keys are ignored; null on a known key means its default.
	parsed_build_settings parse_build_settings(std::string_view json_text);
	// Writes the known keys into `doc` (made an object if it is not one), keeping the other keys and their order.
	void merge_build_settings(boost::json::value& doc, const build_settings& settings);
	// <snk_dir>/build.json; defaults when it is missing (silently) or unreadable (with a warning naming the file).
	build_settings load_build_settings(const std::filesystem::path& snk_dir);

	// Reads <snk_dir>/build.json, lets `change` edit the settings parsed from it, merges them back (unknown keys kept)
	// and writes the file atomically (prj::write_text_atomically), creating <snk_dir>, unless `change` changed nothing.
	// A file that cannot be read or is not a JSON object is an error and is never overwritten. nullopt on success.
	// `write` set: the file is written through it (tests).
	std::optional<std::string> update_build_settings(const std::filesystem::path& snk_dir,
		const std::function<void(build_settings&)>& change, const prj::text_writer& write = {});
	// Why update_build_settings would refuse <snk_dir>/build.json; nullopt when it is missing or a JSON object.
	std::optional<std::string> check_build_settings_file(const std::filesystem::path& snk_dir);
	// Drops `configuration` from fresh_configure of <snk_dir>/build.json (update_build_settings).
	std::optional<std::string> clear_fresh_configure(const std::filesystem::path& snk_dir, std::string_view configuration);
}
