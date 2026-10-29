#pragma once
#include "edt_build_jobs.h"
#include "edt_build_settings.h"
#include <snk/editor/edt_registration.h>
#include <snk/command/cmd_registry.h>
#include <snk/project/prj_cmake_presets.h>
#include <boost/json/value.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	inline constexpr int k_max_manual_jobs = 1024;
	inline constexpr std::string_view k_building_reason = "A build is running: removing a define waits until it ends "
		"(the build would clear the --fresh mark that the removal needs)";

	// user-<cfg> when <code_dir>/CMakeUserPresets.json has it, else `configuration`. A broken user file is logged as a
	// warning and gives `configuration` (the team preset still builds).
	std::string preset_for_configuration(const std::filesystem::path& code_dir, std::string_view configuration);

	// One configure preset of CMakePresets.json as the Build Settings window shows it.
	struct build_settings_configuration
	{
		std::string name;
		// displayName, else the name.
		std::string display_name;
		// Effective, read-only.
		std::string generator;
		// Effective cacheVariables of <name> in CMakePresets.json: read-only.
		std::map<std::string, std::string> team_defines;
		// cacheVariables of user-<name>, with the queued edits.
		std::map<std::string, std::string> user_defines;
	};

	struct build_settings_apply_result
	{
		// Empty when the file was written, had nothing to write, or was not touched.
		std::string user_presets_error;
		std::string build_settings_error;
		// Configurations this Apply marked for a --fresh configure (a user define of theirs was removed).
		std::vector<std::string> fresh;

		bool ok() const { return user_presets_error.empty() && build_settings_error.empty(); }
	};

	// The state behind the Build Settings window (#444), without ImGui: code/CMakePresets.json (read-only),
	// code/CMakeUserPresets.json (the user-<cfg> presets, through prj::save_user_presets) and .snk/build.json.
	// Edits are queued and touch no file until apply(). Main thread only.
	class build_settings_model
	{
	public:
		// Reads the three files of the project at `root` (empty: no project) and drops the queued edits.
		void load(const std::filesystem::path& root);

		// What the editor builds without a configuration in build.json (cfg build/configuration).
		void set_default_configuration(std::string configuration) { m_default_configuration = std::move(configuration); }
		void set_estimate(const jobs_estimate& estimate) { m_estimate = estimate; }
		// Whether a game build runs: removing a define and applying a removal wait for its end, since its successful
		// configure clears fresh_configure. Null: never.
		void set_building(std::function<bool()> building) { m_building = std::move(building); }
		bool is_building() const { return m_building && m_building(); }
		// How the files are written; null: prj::write_text_atomically (tests inject failures).
		void set_writer(prj::text_writer write) { m_write = std::move(write); }

		// Empty when the presets were read; otherwise why the window has nothing to show.
		const std::string& unavailable_reason() const { return m_unavailable; }
		bool available() const { return m_unavailable.empty(); }
		// A broken user presets file or build.json, values of build.json that were not understood.
		const std::vector<std::string>& load_errors() const { return m_load_errors; }

		const std::filesystem::path& root() const { return m_root; }
		std::filesystem::path code_dir() const { return m_root / "code"; }
		std::filesystem::path snk_dir() const { return m_root / ".snk"; }
		std::filesystem::path user_presets_file() const;
		std::filesystem::path build_settings_file() const;

		// The non-hidden configure presets of CMakePresets.json, in its order.
		const std::vector<build_settings_configuration>& configurations() const { return m_configurations; }
		const build_settings_configuration* find(std::string_view configuration) const;
		// build.json's configuration, else the default one.
		std::string selected_configuration() const;
		// With the queued edits; fresh_configure is as loaded.
		const build_settings& settings() const { return m_settings; }
		// The jobs Manual mode writes: the edited value, else preset_jobs(), else the estimate.
		int manual_jobs() const;
		// `jobs` of the build preset user-<selected configuration> as loaded.
		std::optional<int> preset_jobs() const;
		// What the build uses for `configuration`, as loaded (preset_for_configuration without reading the file again).
		std::string preset_for(std::string_view configuration) const;
		const jobs_estimate& estimate() const { return m_estimate; }

		// Validated now (prj::check_user_define); a refused edit is not queued and its error is returned.
		std::optional<std::string> set_define(std::string_view configuration, std::string_view name, std::string_view value);
		// For every configuration, or for none when one of them refuses it.
		std::optional<std::string> set_define_everywhere(std::string_view name, std::string_view value);
		// Refused while a build runs (k_building_reason) or for a define the user preset does not have.
		std::optional<std::string> remove_define(std::string_view configuration, std::string_view name);
		void set_jobs_mode(jobs_mode mode) { m_settings.jobs_mode = mode; }
		// Clamped to [1, k_max_manual_jobs].
		void set_manual_jobs(int jobs);
		void set_configuration(std::optional<std::string> configuration) { m_settings.configuration = std::move(configuration); }
		void set_reload_after_build(bool reload) { m_settings.reload_after_build = reload; }
		void set_log_verbosity(log_verbosity verbosity) { m_settings.log_verbosity = verbosity; }
		void set_live_enabled(bool enabled) { m_settings.live_enabled = enabled; }
		// Clamped to [0, k_max_live_quiet_ms].
		void set_live_quiet_ms(int quiet_ms);
		// Clamped to [0, k_max_live_jobs]; 0: auto.
		void set_live_jobs(int jobs);

		bool dirty() const;
		// Drops the queued edits (the files are not read again).
		void revert();
		// The files apply() would write, in that order.
		std::vector<std::filesystem::path> pending_files() const;
		// The edits apply() hands to prj::save_user_presets.
		std::vector<prj::user_presets_edit> pending_user_edits() const;
		// The configurations apply() would mark for --fresh.
		std::vector<std::string> pending_fresh() const;
		// The files whose content differs from what load() (or the last apply) saw.
		std::vector<std::filesystem::path> changed_on_disk() const;
		// When build.json changed on disk only in fresh_configure (the builder cleared a mark), takes that over
		// silently and returns true.
		bool refresh_fresh_configure();
		// Why apply() would refuse now (a build runs and the edits remove a define); nullopt when it would not.
		std::optional<std::string> apply_blocked_reason() const;

		// Writes build.json (read, merged, written atomically; with the --fresh marks) and then the user presets (read
		// fresh and edited, prj::save_user_presets). A broken build.json is never overwritten; when it cannot be written
		// and the Apply removes a define, the user presets are not written either. Reloads after a full success; on an
		// error the edits stay.
		build_settings_apply_result apply();

	private:
		struct snapshot
		{
			bool exists = false;
			std::string text;
		};

		static snapshot read_snapshot(const std::filesystem::path& file);
		bool settings_changed() const;
		int presets_version() const;

		std::filesystem::path m_root;
		std::string m_default_configuration = "debuggame";
		jobs_estimate m_estimate{};
		std::function<bool()> m_building;
		prj::text_writer m_write;
		std::string m_unavailable = "Open a project first";
		std::vector<std::string> m_load_errors;

		std::optional<prj::main_presets> m_main;
		boost::json::value m_user_doc;
		bool m_user_broken = false;
		std::vector<build_settings_configuration> m_loaded_configurations;
		build_settings m_loaded_settings;

		std::vector<build_settings_configuration> m_configurations;
		build_settings m_settings;
		std::optional<int> m_manual_jobs;
		std::vector<prj::user_presets_edit> m_define_edits;

		snapshot m_main_snapshot;
		snapshot m_user_snapshot;
		snapshot m_settings_snapshot;
	};

	struct build_settings_command_options
	{
		// Nullopt: no project is open.
		std::function<std::optional<std::filesystem::path>()> project_root;
		std::string default_configuration = "debuggame";
		std::function<jobs_estimate()> estimate = estimate_jobs;
	};

	// build.settings.get {} -> {configuration, jobs_mode, jobs: {manual?, estimate: {jobs, cores, ram_bytes}},
	// reload_after_build, log_verbosity, live: {enabled, quiet_ms, jobs}, configurations: {<cfg>: {preset, team_defines,
	// user_defines}}, errors}.
	// Reads the files on every call. Fails with no_project, or failed when the presets cannot be read. Read-only.
	[[nodiscard]] std::vector<registration> register_build_settings_commands(cmd::command_registry& commands,
		build_settings_command_options options);
}
