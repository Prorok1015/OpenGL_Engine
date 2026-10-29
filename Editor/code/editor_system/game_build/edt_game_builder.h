#pragma once
#include "edt_build_settings.h"
#include "edt_toolchain.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_registration.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace edt
{
	class dialog_service;
	class editor_events;
	class job_service;

	// The open project as the builder sees it.
	struct game_project
	{
		std::filesystem::path root;
		// The runtime modules of the manifest: their files are named in the log after a build.
		std::vector<std::string> modules;
	};

	enum class build_priority : std::uint8_t { normal, low };

	// Everything one build needs besides the project; made per build by a profile (make_manual_build_request).
	struct build_request
	{
		// "debuggame" or "release".
		std::string configuration;
		// The configure and build preset; empty: `configuration`.
		std::string preset;
		// `--parallel <jobs>`; 0: not passed (the preset or the generator decides).
		int jobs = 0;
		// low: the build's processes run below normal priority (platform::process_options::low_priority).
		build_priority priority = build_priority::normal;
		// Meeting a running build, cancel it and run once it has stopped (only the latest such request waits);
		// false: the request fails with `busy`.
		bool cancel_on_restart = false;
		log_verbosity verbosity = log_verbosity::normal;
		// Passed on as game_module_build_result::reload.
		bool reload_after = true;
	};

	// std::thread::hardware_concurrency(), at least 1.
	int default_build_jobs();

	struct game_builder_options
	{
		// Nullopt: no project is open.
		std::function<std::optional<game_project>()> project;
		// The folder holding snake_sdkConfig.cmake, given to the build as SNAKE_SDK_DIR.
		std::filesystem::path sdk_dir;
		// "debuggame" or "release": what build.game_module builds without the argument.
		std::string default_configuration = "debuggame";
		// A game module links the engine: only an editor on the shared engine can build (and load) one.
#if defined(SNK_ENGINE_SHARED)
		bool engine_shared = true;
#else
		bool engine_shared = false;
#endif
		toolchain_settings toolchain{};
		process_runner run = platform::run_process;
		// Runs on the worker thread. Null: find_toolchain with `toolchain`, kept for the editor session.
		std::function<std::variant<edt::toolchain, std::string>(std::stop_token)> find_toolchain;
		// Shows the progress window (dialog_service::implicit_registrar); null: none (tests).
		std::function<void(std::string_view id, std::function<bool()> draw)> show_progress;
		// Read at the start of every manual build, so edits of .snk/build.json apply without a restart.
		std::function<build_settings()> settings = [] { return build_settings{}; };
		// The preset name of a configuration.
		std::function<std::string(std::string_view configuration)> preset_for = [](std::string_view configuration) {
			return std::string(configuration);
		};
		// The jobs of a build whose settings say jobs_mode auto.
		std::function<int()> auto_jobs = default_build_jobs;
		// Gets game_module_built; null: none. Must outlive the builder.
		editor_events* events = nullptr;
	};

	// What one build produced; filled on the worker thread.
	struct game_build_result
	{
		// The toolchain or the SDK was not found: text for the person. Nothing ran.
		std::string setup_error;
		// "configure" or "build" when a step failed; empty on success.
		std::string failed_step;
		int exit_code = 0;
		// The step could not be started (platform::process_result::error).
		std::string process_error;
		std::vector<problem> problems;
		struct module_file
		{
			std::string name;
			// Absolute; the first of module_file_names that exists, else the first of them.
			std::filesystem::path path;
			bool exists = false;
		};
		// The files the manifest's modules were built into (<root>/build/bin/<file>).
		std::vector<module_file> modules;
		// The last output lines, for the log.
		std::vector<std::string> tail;
		// Stopped for a cancel_on_restart request: nothing to report.
		bool restarted = false;

		bool ok() const { return setup_error.empty() && failed_step.empty(); }
	};

	// "[n/m]" at the start of a ninja line.
	std::optional<std::pair<int, int>> parse_ninja_progress(std::string_view line);
	// The setup error for an SDK dir without snake_sdkConfig.cmake (`tried` empty: none was found at all).
	std::string sdk_not_found_message(const std::filesystem::path& tried);
	// <name>.dll on Windows, lib<name>.so elsewhere (what the template's add_library(SHARED) produces).
	std::string module_file_name(std::string_view module);
	// module_file_name first, then the other names the module loader accepts (<name>.so on POSIX).
	std::vector<std::string> module_file_names(std::string_view module);

	// The build of Build > Build Game: `configuration` (empty: settings.configuration, else
	// options.default_configuration), options.preset_for, the settings' jobs mode and verbosity, normal priority.
	build_request make_manual_build_request(const build_settings& settings, std::string configuration,
		const game_builder_options& options);

	// Builds the open project's game module (<root>/code, CMake presets) on a job: Build > Build Game, action
	// build.game_module. The output goes to the problem list (source "build"); a failure is shown as a message.
	// Main thread only. Destroy it before the job_service, problem_list, dialog_service and action_registry it uses,
	// and after job_service::cancel_all_and_wait (its jobs reach none of them, only its on_done does).
	class game_builder
	{
	public:
		static constexpr const char* k_action_id = "build.game_module";
		static constexpr std::size_t k_tail_lines = 200;

		game_builder(job_service& jobs, problem_list& problems, dialog_service& dialogs, action_registry& actions,
			game_builder_options options);
		~game_builder();
		game_builder(const game_builder&) = delete;
		game_builder& operator=(const game_builder&) = delete;

		[[nodiscard]] bool is_building() const;
		// The running build's job; 0 when none runs.
		[[nodiscard]] std::uint64_t job() const { return is_building() ? m_job : 0; }
		// Why build.game_module is disabled; empty when it is enabled.
		[[nodiscard]] std::string disabled_reason() const;
		// Result {started, job} like build.game_module; a request queued behind a running build gives started false, job 0.
		// Fails with `disabled`, `busy` (a build runs and the request does not cancel_on_restart) or `invalid_args`.
		action_result start(build_request request);
		// Also drops a request waiting for the running build to stop.
		void cancel();

	private:
		bool has_presets() const;
		void launch(build_request request);
		void finish(game_build_result& result, const build_request& request);
		bool draw_progress();

		job_service& m_jobs;
		problem_list& m_problems;
		dialog_service& m_dialogs;
		game_builder_options m_options;
		std::shared_ptr<toolchain_cache> m_toolchains = std::make_shared<toolchain_cache>();
		std::uint64_t m_job = 0;
		// Stops the running build for a restart without cancelling its job, so its on_done still comes.
		std::shared_ptr<std::stop_source> m_restart_stop;
		std::optional<build_request> m_pending;
		std::string m_configuration;
		float m_spinner_t = 0.f;
		bool m_progress_shown = false;
		// The presets check runs on every frame (shortcut dispatch reads `enabled`): one stat per second is enough.
		mutable std::chrono::steady_clock::time_point m_presets_checked{};
		mutable bool m_presets_found = false;
		// Seen by the progress window, which the editor layer may still call after this is gone.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		registration m_action;
		// Last: dies first, so no on_done reaches a half-destroyed builder.
		ds::lifetime m_lifetime{ ds::lifetime_option::cancel_on_destroy };
	};
}
