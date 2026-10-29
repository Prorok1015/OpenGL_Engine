#pragma once
#include "edt_build_settings.h"
#include "edt_toolchain.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
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
	class job_service;

	// The open project as the builder sees it.
	struct game_project
	{
		std::filesystem::path root;
		// The runtime modules of the manifest: their files are named in the log after a build.
		std::vector<std::string> modules;
	};

	enum class build_priority : std::uint8_t { normal, low };
	// How a build tells the person about itself. problems_only (live builds): no message dialog, the output tail of a
	// failure and the start and success lines at debug; the problems go to the list as always.
	enum class build_report : std::uint8_t { dialog, problems_only };

	// How one build_request ended, for its on_finished.
	struct build_outcome
	{
		// restarted: stopped for a newer cancel_on_restart request, which runs next. preempted: a preemptible build
		// stopped for a request that is not (the person's build), which runs next.
		enum kind_t { succeeded, failed, restarted, preempted };
		kind_t kind = failed;
		std::uint64_t job = 0;
		int errors = 0;
		int warnings = 0;
		// succeeded: one per module file that exists, as game_module_built gets them.
		std::vector<game_module_build_result> modules;
		// failed: the setup error or the failure summary.
		std::string message;
		// failed: a request that waited for a running build to stop could not start then (`message` says why).
		bool not_started = false;
	};

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
		// false: the request fails with `busy`. A preemptible request never stops a build that is not (`busy` then).
		bool cancel_on_restart = false;
		// A background build (live mode) that yields: a request that is not preemptible stops it and runs once it has
		// stopped, as cancel_on_restart does. A preemptible request meeting a build that is not stays `busy`.
		bool preemptible = false;
		log_verbosity verbosity = log_verbosity::normal;
		// Passed on as game_module_build_result::reload.
		bool reload_after = true;
		// Configure with --fresh (CMake 3.24+): the configuration is in build_settings::fresh_configure.
		bool fresh = false;
		// build_settings::fresh_generation when the request was made: handed to on_fresh_configured.
		std::uint64_t fresh_generation = 0;
		build_report report = build_report::dialog;
		// false: no progress window is opened for this request (one already open stays).
		bool show_progress = true;
		// On the main thread when the build of this request ends. Not called for a job cancelled from outside
		// (job.cancel, game_builder::cancel): job_service skips its on_done.
		std::function<void(const build_outcome&)> on_finished;
	};

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
		// The jobs of a build whose settings say jobs_mode auto (at least 1 is passed); null: no `--parallel`, the preset decides.
		std::function<int()> auto_jobs;
		// Gets game_module_built; null: none. Must outlive the builder.
		editor_events* events = nullptr;
		// On the main thread, after a `fresh` request's configure step succeeded (also when the build step then fails):
		// the configuration no longer needs --fresh, unless it was marked again after `generation` (the request's
		// fresh_generation). Null: nothing is recorded.
		std::function<void(std::string_view configuration, std::uint64_t generation)> on_fresh_configured;
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
		// The configure step succeeded.
		bool configured = false;

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
	// options.default_configuration), options.preset_for, the settings' jobs mode and verbosity, normal priority;
	// fresh when the configuration is in settings.fresh_configure.
	build_request make_manual_build_request(const build_settings& settings, std::string configuration,
		const game_builder_options& options);
	// A background build of live mode (#445): settings.configuration else options.default_configuration,
	// options.preset_for, live_jobs (0: min(2, auto_jobs), at least 1), low priority, cancel_on_restart, preemptible, the settings'
	// verbosity, problems_only without a progress window, no reload (live mode reloads after its quiet period);
	// fresh like the manual build.
	build_request make_live_build_request(const build_settings& settings, const game_builder_options& options);

	// Builds the open project's game module (<root>/code, CMake presets) on a job: Build > Build Game, action
	// build.game_module. The output goes to the problem list (source "build"); a failure is shown as a message
	// (build_report::dialog).
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
		// A preemptible (live) build runs: a manual request stops it rather than being `busy`.
		[[nodiscard]] bool building_preemptible() const { return is_building() && m_running_preemptible; }
		// A build the person started runs (what greys out reloads and define removal).
		[[nodiscard]] bool is_manual_build_running() const { return is_building() && !m_running_preemptible; }
		// The running build's job; 0 when none runs.
		[[nodiscard]] std::uint64_t job() const { return is_building() ? m_job : 0; }
		// Why build.game_module is disabled; empty when it is enabled.
		[[nodiscard]] std::string disabled_reason() const;
		// What make_manual_build_request / make_live_build_request read (settings, preset_for, auto_jobs).
		[[nodiscard]] const game_builder_options& options() const { return m_options; }
		// Result {started, job} like build.game_module; a request queued behind a running build gives started false, job 0.
		// Fails with `disabled`, `busy` (a build runs and neither the request's cancel_on_restart nor the running build's
		// preemptible lets it through) or `invalid_args`.
		action_result start(build_request request);
		// Also drops a request waiting for the running build to stop.
		void cancel();
		// Stops a running preemptible (live) build the way a preempting request does: its on_done still comes, with
		// outcome `preempted`. A stopped build reports no configure (its result has `configured` false), so a --fresh
		// mark it was configuring for stays: one extra fresh configure, never a lost mark. A preemptible request waiting
		// for it is dropped (told `preempted`); a waiting request of the person's stays and starts once it has stopped.
		// Nothing when no preemptible build runs.
		void stop_background();

	private:
		bool has_presets() const;
		void launch(build_request request);
		build_outcome finish(game_build_result& result, const build_request& request);
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
		// Of the running (or last) build.
		bool m_running_preemptible = false;
		// The running build was stopped for a request that is not preemptible.
		bool m_preempted = false;
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
