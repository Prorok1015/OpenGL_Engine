#pragma once
#include "edt_toolchain.h"
#include <snk/common/ds/ds_task_system.hpp>
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
	class action_registry;
	class dialog_service;
	class job_service;

	// The open project as the builder sees it.
	struct game_project
	{
		std::filesystem::path root;
		// The runtime modules of the manifest: their files are named in the log after a build.
		std::vector<std::string> modules;
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
		// The files the manifest's modules were built into (<root>/build/bin/<file>).
		std::vector<std::filesystem::path> modules;
		// The last output lines, for the log.
		std::vector<std::string> tail;

		bool ok() const { return setup_error.empty() && failed_step.empty(); }
	};

	// "[n/m]" at the start of a ninja line.
	std::optional<std::pair<int, int>> parse_ninja_progress(std::string_view line);
	// <name>.dll on Windows, lib<name>.so elsewhere (what the template's add_library(SHARED) produces).
	std::string module_file_name(std::string_view module);

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
		void cancel();

	private:
		bool has_presets() const;
		void finish(game_build_result& result);
		bool draw_progress();

		job_service& m_jobs;
		problem_list& m_problems;
		dialog_service& m_dialogs;
		game_builder_options m_options;
		std::shared_ptr<toolchain_cache> m_toolchains = std::make_shared<toolchain_cache>();
		std::uint64_t m_job = 0;
		std::string m_configuration;
		float m_spinner_t = 0.f;
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
