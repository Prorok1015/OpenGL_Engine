#pragma once
#include "edt_build_settings.h"
#include "edt_live_build.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_file_change.h>
#include <boost/json/object.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }

namespace edt
{
	// The changes of code:// as live mode sees them. Relevant: a file (not a folder) that is code (prj::code_category
	// source, header or cmake, CMakePresets.json and CMakeUserPresets.json included); a rename when either name is.
	// Changes done through the resource system (`self`) count: agents write the sources that way.
	std::vector<source_change> source_changes_of(std::span<const res::file_change> changes);

	// "Live: <state>" for the menu bar. `reload_clock`: the hh:mm:ss shown after "reloaded" (empty: none).
	std::string live_status_text(const live_status& status, std::string_view reload_clock = {});

	// live.status: {enabled, state, errors, warnings, configuration, job, last_build_end_seconds_ago?,
	// last_reload_seconds_ago?, message}.
	boost::json::object live_status_json(bool enabled, const live_status& status, std::chrono::steady_clock::time_point now);

	struct live_mode_options
	{
		// For live_build; `now` is also the clock of live.status.
		live_build_deps build;
		// Why live mode cannot be turned on now; empty: it can. Null: it always can.
		std::function<std::string()> unavailable_reason;
		// Writes live_enabled into build.json; the error, nullopt when it is stored. Null: not stored.
		std::function<std::optional<std::string>(bool enabled)> store_enabled;
		// After live mode was turned on or off: the tools an agent sees changed.
		std::function<void()> on_toggled;
		// Items of the status popup; null: not shown.
		std::function<void()> open_problems;
		std::function<void()> open_settings;
	};

	// Live mode in the editor (#445): owns the live_build, feeds it the code:// changes and a tick per frame, and
	// registers Build > Live Mode (build.live.toggle, not a tool) and the agent tools live.status, live.rebuild_now and
	// live.reload_now. Those three are `agent_when_enabled`: an agent sees and calls them only while live mode is on (the
	// person's consent, AI_INTEGRATION.md section 5). Main thread only.
	class live_mode
	{
	public:
		static constexpr const char* k_toggle_id = "build.live.toggle";

		live_mode(problem_list& problems, action_registry& actions, live_mode_options options);
		~live_mode();
		live_mode(const live_mode&) = delete;
		live_mode& operator=(const live_mode&) = delete;

		// Takes the code:// changes of `resources` until this is destroyed. False (nothing watched) without a code:// mount.
		bool watch(res::resource_system& resources);

		[[nodiscard]] bool enabled() const { return m_build.enabled(); }
		[[nodiscard]] std::string unavailable_reason() const;
		// `store`: build.json too (a failed write is logged; the switch still flips for this session). Turning on while
		// unavailable is refused with the reason.
		std::optional<std::string> set_enabled(bool enabled, bool store);
		// build.json was applied by the Build Settings window: follow its live_enabled.
		void follow_settings(const build_settings& settings);

		void on_changes(std::span<const res::file_change> changes);
		// Every frame, outside the job pump.
		void tick();
		// Into the open main menu bar, right-aligned; draws nothing at all while live mode is off.
		void draw_status();

		[[nodiscard]] const live_status& status() const { return m_build.status(); }
		live_build& build() { return m_build; }

	private:
		void register_commands(action_registry& actions);
		// hh:mm:ss of the last reload, local time.
		const std::string& reload_clock();

		live_mode_options m_options;
		std::function<std::chrono::steady_clock::time_point()> m_now;
		live_build m_build;
		res::resource_system* m_watched = nullptr;
		std::optional<std::chrono::steady_clock::time_point> m_clock_of;
		std::string m_clock_text;
		// Last: dropped first, so no command outlives what it calls.
		std::vector<registration> m_registrations;
	};
}
