#pragma once
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <snk/project/prj_project.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class action_registry;
	class dialog_service;

	// The modules a build reports and a reload may take (#664): the project's runtime modules first, in manifest order, then its
	// editor modules, so a reload without a name keeps taking the game module.
	std::vector<std::string> project_module_names(const prj::project_context& project);

	struct game_reloader_options
	{
		// The modules of the open project's manifest: the runtime ones first, then the editor ones (#664); empty without a
		// project. A reload without a module name takes the first of them that has a build.
		std::function<std::vector<std::string>()> modules;
		// `module` is an editor module (kind "editor"): it reloads on its own, with no Play snapshot and no swap gate, and is
		// not reloaded when it is the build already in use. Null: none is.
		std::function<bool(const std::string& module)> is_editor_module;
		// The running copy of `module` was made from the build output as it is now (project_module_host::is_current). Used for
		// editor modules only: the reload of the game module reloaded them already. Null: never.
		std::function<bool(const std::string& module)> is_current;
		// Reloading `module` unloads editor modules (an editor module, or any module while editor modules are loaded): the
		// action then never reloads inside the call, which may come from one of their callbacks (a panel's button), but on the
		// next UI frame (each_frame), as for a live build. Null: never.
		std::function<bool(const std::string& module)> unloads_editor_modules;
		// The build of a module in the project's build output, if there is one.
		std::function<std::optional<std::filesystem::path>(const std::string& module)> build_output;
		// Swaps the running module for its new build (core::project_module_host::reload with the editor's hooks).
		std::function<core::reload_report(const std::string& module)> reload;
		// The same for a reload the person asked for (the action, the reload after their build): it may load a build the
		// host took back (#601), live mode's never. Null: `reload`.
		std::function<core::reload_report(const std::string& module)> reload_by_person;
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
		// each_frame id of the one reload of a module waiting for a build or `defer` (+ the module name): not the one
		// above, which may be running the frame that queues it, and register_implicit ignores a taken id.
		static constexpr std::string_view k_wait_frame_id = "edt/reload_game_module/waiting/";

		game_reloader(dialog_service& dialogs, action_registry& actions, game_reloader_options options);

		// Why build.reload_game_module is disabled; empty when it is enabled. Looks at the disk at most once a second.
		[[nodiscard]] std::string disabled_reason() const;
		// Empty `module`: the first runtime module that has a build, else every editor module that has one (a build of
		// `game_editor` alone); a module named is reloaded whatever else is built, current or not.
		core::reload_report reload(const std::string& module);
		// reload without the message for a build not taken: the caller reports it (live mode, #445). An editor module that is
		// the build in use is not reloaded again ("already up to date"): live mode asks for every module a build made.
		core::reload_report reload_quiet(const std::string& module);
		// Both of the above refuse while `defer` names a reason. These wait instead: now, or on the first frame without
		// a reason (each_frame), and hand the report to `done`. Without each_frame: now. Requests for a module that
		// already waits join that wait: one swap, every `done` gets its report, and the messages are shown when any
		// request was not quiet. A reloader destroyed while a reload waits never calls its `done`.
		// `only_if_stale`: an editor module that is the build in use is skipped (the reload after a build asks for every
		// module the build made, and the reload of the game module reloaded the editor modules already); a request of the
		// person's (false) that joins the wait makes it a real reload.
		void reload_when_ready(const std::string& module, bool only_if_stale = false);
		void reload_quiet_when_ready(const std::string& module, std::function<void(const core::reload_report&)> done);
		// Why a reload must wait now; empty: it may run.
		[[nodiscard]] std::string defer_reason() const;

	private:
		// The built modules a request for `module` reloads: the one named; none named: the first runtime module, else every editor module.
		std::vector<std::string> targets(const std::string& module) const;
		bool is_editor(const std::string& module) const;
		// A build runs, a live build is being stopped or `defer` names a reason: the reload is refused for now.
		std::optional<core::reload_report> holding_back() const;
		core::reload_report reload_one(const std::string& module, bool by_person, bool skip_current);
		// All the targets of `module`: the first report that was not `reloaded`, else the last. Each is told to the person when `tell_it`.
		core::reload_report run(const std::string& module, bool tell_it, bool skip_current);
		core::reload_report tell(const std::string& module, core::reload_report report);
		// The action during a live build or while `defer` names a reason: stops the live build and queues the reload
		// until nothing holds it. {status: queued} or the refusal.
		action_result queue_action_reload(const std::string& module);
		// Joins or starts the wait of `module` (k_wait_frame_id): polls until neither a build nor `defer` holds it.
		void wait_then_reload(const std::string& module, bool quiet, std::function<void(const core::reload_report&)> done, bool only_if_stale);

		struct waiting_reload
		{
			bool tell = false;
			bool only_if_stale = true;
			std::vector<std::function<void(const core::reload_report&)>> done{};
		};
		bool any_build_running() const;
		// A reload of the module the action would pick already waits.
		bool waits(const std::string& module) const;

		dialog_service& m_dialogs;
		game_reloader_options m_options;
		registration m_action;
		connection m_built;
		// What a frame scheduled by a build checks: the reloader may be gone by then.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
		std::map<std::string, waiting_reload> m_waiting;
		mutable std::chrono::steady_clock::time_point m_build_checked{};
		mutable bool m_build_found = false;
	};
}
