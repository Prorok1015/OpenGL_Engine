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
		// a taken id). Null: a reload during a live build is refused instead of queued.
		std::function<void(const std::string& id, std::function<bool()> poll)> each_frame;
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

		game_reloader(dialog_service& dialogs, action_registry& actions, game_reloader_options options);

		// Why build.reload_game_module is disabled; empty when it is enabled. Looks at the disk at most once a second.
		[[nodiscard]] std::string disabled_reason() const;
		// Empty `module`: the first runtime module that has a build.
		core::reload_report reload(const std::string& module);
		// reload without the message for a build not taken: the caller reports it (live mode, #445).
		core::reload_report reload_quiet(const std::string& module);

	private:
		std::optional<std::string> pick(const std::string& module) const;
		// `who` gets the module the report is about: the picked one, else `module`.
		core::reload_report attempt(const std::string& module, std::string& who);
		core::reload_report tell(const std::string& module, core::reload_report report);
		// The action during a live build: stops it and queues the reload until no build runs. {status: queued} or the
		// refusal.
		action_result reload_after_background(const std::string& module);
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
