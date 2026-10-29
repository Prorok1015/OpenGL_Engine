#pragma once
#include "edt_build_settings.h"
#include "edt_game_builder.h"
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_problem_list.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	// One changed source file as live mode sees it; the editor maps its file events onto `relevant`.
	struct source_change
	{
		std::string path;
		// A change that needs a rebuild (code of the game module).
		bool relevant = false;
	};

	enum class live_state : std::uint8_t
	{
		off,
		idle,
		// Changes wait for the debounce, or for a manual build to end.
		pending,
		building,
		// The last live build failed, or could not start (`message`).
		errors,
		// Built; waits until the sources stay unchanged for live_quiet_ms.
		settling,
		// Inside the reload call only.
		reloading,
		reloaded,
		reload_failed,
	};

	std::string_view to_string(live_state state);

	struct live_status
	{
		live_state state = live_state::off;
		// Of the last finished live build.
		int errors = 0;
		int warnings = 0;
		std::string configuration;
		// The running or last finished live build's job; 0 while a restarted request waits for its job.
		std::uint64_t job = 0;
		std::optional<std::chrono::steady_clock::time_point> last_build_end;
		std::optional<std::chrono::steady_clock::time_point> last_reload;
		std::string message;
	};

	struct live_build_deps
	{
		std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
		std::function<build_settings()> settings = [] { return build_settings{}; };
		// Null: make_live_build_request with default builder options; the editor passes its builder's.
		std::function<build_request(const build_settings&)> make_request;
		// game_builder::start.
		std::function<action_result(build_request)> start_build;
		// game_builder::is_building; any build, the person's too.
		std::function<bool()> is_building;
		// game_builder::cancel.
		std::function<void()> cancel_build;
		// game_reloader::reload_quiet: no dialog.
		std::function<core::reload_report(const std::string& module)> reload;
	};

	// Live mode (#445): rebuilds the game module after a batch of relevant source changes and reloads it once the
	// sources stay quiet. A pure state machine over injected dependencies; the editor feeds on_changes and calls tick()
	// every frame. A failed reload shows no dialog: it goes to the problems under source "reload". Main thread only.
	class live_build
	{
	public:
		static constexpr std::chrono::milliseconds k_debounce{ 500 };
		static constexpr std::string_view k_problem_source = "reload";

		live_build(problem_list& problems, live_build_deps deps);
		~live_build();
		live_build(const live_build&) = delete;
		live_build& operator=(const live_build&) = delete;

		// Off cancels a running live build (never the person's) and forgets what was pending.
		void set_enabled(bool enabled);
		[[nodiscard]] bool enabled() const { return m_enabled; }
		// Ignored when off; a batch without a relevant change is ignored.
		void on_changes(std::span<const source_change> changes);
		void tick();
		// Starts a build now, skipping the debounce; behind the person's build it waits (pending). nullopt: started or queued.
		std::optional<std::string> rebuild_now();
		// Reloads now, skipping the quiet period: only after a successful live build with no change since it started.
		// nullopt: reloaded.
		std::optional<std::string> reload_now();
		[[nodiscard]] const live_status& status() const { return m_status; }

	private:
		struct finished
		{
			std::uint64_t attempt = 0;
			build_outcome outcome;
		};
		// Filled by on_finished, which may come after the controller is gone (it holds a weak_ptr).
		using inbox = std::vector<finished>;

		void take_outcomes();
		void handle(const build_outcome& outcome);
		// The start error, unless it is `busy` (stays pending).
		std::optional<std::string> try_start();
		bool quiet_enough() const;
		std::optional<std::string> reload_built();
		void warn_once(const std::string& message);

		problem_list& m_problems;
		live_build_deps m_deps;
		live_status m_status;
		bool m_enabled = false;
		// Relevant changes not yet in a started build.
		bool m_dirty = false;
		// rebuild_now: the next start skips the debounce.
		bool m_force = false;
		std::optional<std::chrono::steady_clock::time_point> m_last_change;
		// Bumped per relevant batch: a build is reloaded only when it saw the last change.
		std::uint64_t m_change_gen = 0;
		std::uint64_t m_build_gen = 0;
		// Every started request gets the next one; an outcome of another is stale.
		std::uint64_t m_attempt = 0;
		// A live build is running (its outcome has not come yet).
		bool m_ours = false;
		int m_quiet_ms = k_default_live_quiet_ms;
		// The modules of the last successful live build; set while it may still be reloaded.
		std::optional<std::vector<game_module_build_result>> m_built;
		bool m_reload_due = false;
		std::string m_warned;
		std::shared_ptr<inbox> m_inbox = std::make_shared<inbox>();
	};
}
