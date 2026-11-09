#pragma once
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class action_registry;
	class dialog_service;

	struct game_reloader_options
	{
		// The runtime modules of the open project's manifest; empty without a project.
		std::function<std::vector<std::string>()> modules;
		// The build of a module in the project's build output, if there is one.
		std::function<std::optional<std::filesystem::path>(const std::string& module)> build_output;
		// Swaps the running module for its new build (core::project_module_host::reload with the editor's hooks).
		std::function<core::reload_report(const std::string& module)> reload;
		// The person's build is running (greys the action out, refuses a reload); null: never.
		std::function<bool()> building;
		// A background (live) build is running; null: never. It does not grey the action out: the action stops it
		// through stop_background (live mode builds again later) and queues the reload through `each_frame` until no
		// build runs, since the stopping build may still write the module file.
		std::function<bool()> background_building;
		std::function<void()> stop_background;
		// game_module_built is taken from here; null: no reload after a build.
		editor_events* events = nullptr;
		// Runs `fn` once on the next UI frame under `id`: a build finishes in the job pump, and the module must not be
		// swapped under it.
		std::function<void(const std::string& id, std::function<void()> fn)> next_frame;
		// Runs `poll` on every UI frame under `id` while it returns true (editor_layer::register_implicit, which ignores
		// a taken id). Null: a reload during a live build or while `defer` names a reason is refused instead of queued.
		std::function<void(const std::string& id, std::function<bool()> poll)> each_frame;
		// Why a reload must wait now (a level or an asset is loading, #600); empty: it may run. Null: never waits.
		std::function<std::string()> defer;
		// Runs right before the swap, after every check (HOT_RELOAD_PLAY.md 3.1): Play takes its snapshot here.
		// Returns why Play is restarted instead of kept; empty: kept, or not running. Null: nothing runs.
		std::function<std::string(const std::string& module)> before_reload;
	};

	// Reloads the game module from its new build (#435): Build > Reload Game Module (action build.reload_game_module)
	// and after every successful build marked `reload` (editor_events::game_module_built). A build that is not taken is
	// shown as a message; the running module stays. Main thread only, never from one of the module's callbacks.
	class game_reloader
	{
	public:
		static constexpr const char* k_action_id = "build.reload_game_module";
		// next_frame / each_frame id of a reload waiting for a frame: + the module name.
		static constexpr std::string_view k_reload_frame_id = "edt/reload_game_module/";
		// each_frame id of a reload waiting for `defer` (+ the module name): not the one above, which may be running the
		// frame that queues it, and register_implicit ignores a taken id.
		static constexpr std::string_view k_wait_frame_id = "edt/reload_game_module/waiting/";
		// The same for reload_quiet_when_ready: its `done` must never be dropped by a taken id.
		static constexpr std::string_view k_quiet_wait_frame_id = "edt/reload_game_module/waiting_quiet/";

		game_reloader(dialog_service& dialogs, action_registry& actions, game_reloader_options options);

		// Why build.reload_game_module is disabled; empty when it is enabled. Looks at the disk at most once a second.
		[[nodiscard]] std::string disabled_reason() const;
		// Empty `module`: the first runtime module that has a build.
		core::reload_report reload(const std::string& module);
		// reload without the message for a build not taken: the caller reports it (live mode, #445).
		core::reload_report reload_quiet(const std::string& module);
		// Both of the above refuse while `defer` names a reason. These wait instead: now, or on the first frame without
		// a reason (each_frame), and hand the report to `done`. Without each_frame: now.
		void reload_when_ready(const std::string& module);
		void reload_quiet_when_ready(const std::string& module, std::function<void(const core::reload_report&)> done);
		// Why a reload must wait now; empty: it may run.
		[[nodiscard]] std::string defer_reason() const;

	private:
		std::optional<std::string> pick(const std::string& module) const;
		// `who` gets the module the report is about: the picked one, else `module`.
		core::reload_report attempt(const std::string& module, std::string& who);
		core::reload_report tell(const std::string& module, core::reload_report report);
		// The action during a live build or while `defer` names a reason: stops the live build and queues the reload
		// until nothing holds it. {status: queued} or the refusal.
		action_result queue_action_reload(const std::string& module);
		// Polls under `id` until neither a build nor `defer` holds the reload, then reloads `module`.
		void wait_then_reload(const std::string& id, const std::string& module, bool quiet, std::function<void(const core::reload_report&)> done);
		bool any_build_running() const;

		dialog_service& m_dialogs;
		game_reloader_options m_options;
		registration m_action;
		connection m_built;
		// What a frame scheduled by a build checks: the reloader may be gone by then.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		mutable std::chrono::steady_clock::time_point m_build_checked{};
		mutable bool m_build_found = false;
	};
}
