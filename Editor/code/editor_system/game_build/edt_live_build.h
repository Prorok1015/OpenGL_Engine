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
		// Reloads were requested; waits until every module has reported.
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

	enum class reload_now_kind : std::uint8_t
	{
		// The reloads were requested; the result shows in status().
		requested,
		// Every module already runs from the file build/bin has now: nothing was requested.
		up_to_date,
		// Not allowed now (`message` says why).
		refused,
	};

	struct reload_now_result
	{
		reload_now_kind kind = reload_now_kind::refused;
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
		// game_builder::stop_background: stops a live build so its outcome still comes (`preempted`) and a request of
		// the person's waiting for it stays queued. Never game_builder::cancel, which drops that request.
		std::function<void()> stop_background;
		// Asks for game_reloader::reload_quiet(module) on the next UI frame (next_frame, one id per module, like the
		// reload of #435: only there is the module swap safe) and calls `done` with its report. Never `done` in the call.
		std::function<void(const std::string& module, std::function<void(const core::reload_report&)> done)> reload;
		// The copy in use was made from the build/bin file as it is now (core::project_module_host::is_current).
		// Null: never, so every reload is requested.
		std::function<bool(const std::string& module)> is_loaded_current;
		// The project's runtime modules: what reload_now looks at before any live build succeeded. Null: none.
		std::function<std::vector<std::string>()> modules;
		// A module reload is queued for the next UI frame (#435's or live mode's); no build starts meanwhile, since it
		// would refuse the reload. Null: never.
		std::function<bool()> reload_pending;
	};

	// Live mode (#445): rebuilds the game module after a batch of relevant source changes and reloads it once the
	// sources stay quiet. A pure state machine over injected dependencies; the editor feeds on_changes and calls tick()
	// every frame. A failed reload shows no dialog: it goes to the problems under source "reload". Main thread only.
	class live_build
	{
	public:
		static constexpr std::chrono::milliseconds k_debounce{ 500 };
		static constexpr std::string_view k_problem_source = "reload";
		static constexpr std::chrono::milliseconds k_quiet_refresh{ 200 };
		static constexpr std::string_view k_waiting_message = "waiting for the running build";

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
		// Requests now, skipping the quiet period, the reload of each module (those of the last successful live build,
		// else deps.modules) whose build/bin file differs from the running copy; up_to_date when none does. Refused
		// when off, while a build runs or while reloads are still waiting for their result.
		reload_now_result reload_now();
		// A build that was not live mode's succeeded (the person's): its errors are gone, so `errors` turns `idle`.
		void on_manual_build_succeeded();
		[[nodiscard]] const live_status& status() const { return m_status; }

	private:
		struct finished
		{
			std::uint64_t attempt = 0;
			build_outcome outcome;
		};
		// Filled by on_finished, which may come after the controller is gone (it holds a weak_ptr).
		using inbox = std::vector<finished>;

		// The reloads requested at once. A late round still updates Problems unless a newer round has finished; it
		// moves the state only if no change, build or other round came after it was requested (`stale`).
		struct reload_round
		{
			std::uint64_t id = 0;
			std::size_t waiting = 0;
			std::vector<problem> failures;
			bool stale = false;
		};

		// True when an outcome of the current request was handled (that build has just ended).
		bool take_outcomes();
		void handle(const build_outcome& outcome);
		// The start error, unless it is `busy` (stays pending).
		std::optional<std::string> try_start();
		bool quiet_enough() const;
		// live_quiet_ms as build.json has it now, read at most every k_quiet_refresh while settling.
		void refresh_quiet_ms();
		// Whether a build may start in this tick: none runs or is being given way to, none ended in this tick and no
		// reload waits. Otherwise the state turns pending with the reason.
		bool may_start(bool build_ended);
		// The auto reload after settling: the modules of m_built.
		void reload_built();
		// False when every module is current (nothing requested).
		bool request_reloads(const std::vector<std::string>& modules);
		void on_reload_result(std::uint64_t round, const std::string& module, const core::reload_report& report);
		void mark_rounds_stale();
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
		std::optional<std::chrono::steady_clock::time_point> m_quiet_read;
		// is_building() as the last tick saw it: a build that ended since gets one more frame for its reload.
		bool m_was_building = false;
		// The modules of the last successful live build; set while it may still be reloaded.
		std::optional<std::vector<game_module_build_result>> m_built;
		bool m_reload_due = false;
		// The module names of the last successful live build, also after a later change.
		std::vector<std::string> m_last_modules;
		std::vector<reload_round> m_rounds;
		std::uint64_t m_round_id = 0;
		std::uint64_t m_finished_round = 0;
		std::string m_warned;
		std::shared_ptr<inbox> m_inbox = std::make_shared<inbox>();
		// Seen by the reload callbacks, which come a frame later.
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
