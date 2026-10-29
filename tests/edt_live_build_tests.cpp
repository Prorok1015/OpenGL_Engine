#include <boost/test/unit_test.hpp>
#include "edt_live_build.h"
#include <snk/editor/edt_problem_list.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <boost/json/object.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Live mode (#445): live_build over a fake clock, a fake builder and a fake reloader; nothing is built.

namespace {

using namespace std::chrono_literals;

struct live_fixture
{
	live_fixture()
	{
		log = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (category != "editor/build" || level != engine::log_level::warn) return;
			const std::lock_guard lock(log_mutex);
			warn_lines.emplace_back(message);
		});
	}

	edt::live_build_deps deps()
	{
		edt::live_build_deps out;
		out.now = [this] { return now; };
		out.settings = [this] { return settings; };
		out.make_request = [](const edt::build_settings& chosen) {
			return edt::make_live_build_request(chosen, edt::game_builder_options{});
		};
		out.start_build = [this](edt::build_request request) {
			starts.push_back(std::move(request));
			if (next_start.ok) building = true;
			return next_start;
		};
		out.is_building = [this] { return building; };
		out.cancel_build = [this] {
			++cancels;
			building = false;
		};
		out.reload = [this](const std::string& module, std::function<void(const core::reload_report&)> done) {
			reloads.push_back(module);
			waiting.push_back({ module, std::move(done) });
		};
		out.is_loaded_current = [this](const std::string& module) { return current.contains(module); };
		out.modules = [this] { return project_modules; };
		return out;
	}

	void make(bool enabled = true)
	{
		live = std::make_unique<edt::live_build>(problems, deps());
		live->set_enabled(enabled);
	}

	void change(bool relevant = true)
	{
		const edt::source_change item{ "code://src/game_module.cpp", relevant };
		live->on_changes(std::span<const edt::source_change>(&item, 1));
	}

	void advance(std::chrono::milliseconds by)
	{
		now += by;
		live->tick();
	}

	// What game_builder does when the build of starts[index] ends.
	void finish(std::size_t index, edt::build_outcome::kind_t kind, int errors = 0)
	{
		BOOST_REQUIRE(index < starts.size());
		edt::build_outcome outcome;
		outcome.kind = kind;
		outcome.job = 40 + index;
		outcome.errors = errors;
		if (kind == edt::build_outcome::succeeded) {
			outcome.modules.push_back(edt::game_module_build_result{ "game", "build/bin/game.dll", "debuggame", outcome.job, false });
		}
		if (kind != edt::build_outcome::restarted) building = false;
		BOOST_REQUIRE(starts[index].on_finished);
		starts[index].on_finished(outcome);
	}

	// A change, its build, success, the quiet period: the module is reloaded (or not) by the end.
	void build_and_settle()
	{
		const std::size_t index = starts.size();
		change();
		advance(edt::live_build::k_debounce);
		BOOST_REQUIRE_EQUAL(starts.size(), index + 1);
		finish(index, edt::build_outcome::succeeded);
		advance(std::chrono::milliseconds(settings.live_quiet_ms));
		deliver();
	}

	// The next UI frame: every requested reload reports next_reload; a reloaded module runs from build/bin as it is.
	void deliver()
	{
		auto due = std::move(waiting);
		waiting.clear();
		for (auto& item : due) {
			if (next_reload.status == core::reload_status::reloaded) current.insert(item.module);
			item.done(next_reload);
		}
	}

	edt::live_state state() const { return live->status().state; }

	std::vector<edt::problem> reload_problems() const
	{
		std::vector<edt::problem> out;
		for (const auto& item : problems.list()) {
			if (item.source == "reload") out.push_back(item.item);
		}
		return out;
	}

	std::size_t warned(std::string_view text)
	{
		const std::lock_guard lock(log_mutex);
		return static_cast<std::size_t>(std::count_if(warn_lines.begin(), warn_lines.end(),
			[text](const std::string& line) { return line.find(text) != std::string::npos; }));
	}

	std::chrono::steady_clock::time_point now{ std::chrono::seconds(100) };
	edt::build_settings settings;
	bool building = false;
	std::vector<edt::build_request> starts;
	edt::action_result next_start = edt::action_result::success(boost::json::object{ { "started", true }, { "job", 7 } });
	int cancels = 0;
	// Requested reloads, in order.
	std::vector<std::string> reloads;
	struct waiting_reload
	{
		std::string module;
		std::function<void(const core::reload_report&)> done;
	};
	std::vector<waiting_reload> waiting;
	std::set<std::string> current;
	std::vector<std::string> project_modules;
	core::reload_report next_reload{ core::reload_status::reloaded, {} };
	edt::problem_list problems;
	std::unique_ptr<edt::live_build> live;

	std::mutex log_mutex;
	std::vector<std::string> warn_lines;
	engine::log_sink_handle log;
};

bool is(edt::live_state actual, edt::live_state expected)
{
	if (actual != expected) BOOST_TEST_MESSAGE("state is " << edt::to_string(actual));
	return actual == expected;
}

}

BOOST_FIXTURE_TEST_SUITE(edt_live_build_tests, live_fixture)

BOOST_AUTO_TEST_CASE(a_build_starts_500ms_after_the_last_change)
{
	make();
	BOOST_CHECK(is(state(), edt::live_state::idle));
	change();
	BOOST_CHECK(is(state(), edt::live_state::pending));
	advance(300ms);
	change();
	advance(400ms); // 700 ms after the first change, 400 after the last
	BOOST_CHECK(starts.empty());
	advance(99ms);
	BOOST_CHECK(starts.empty());
	advance(1ms);
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	BOOST_CHECK(is(state(), edt::live_state::building));
	BOOST_CHECK_EQUAL(live->status().job, 7u);
	BOOST_CHECK_EQUAL(live->status().configuration, "debuggame");

	const auto& request = starts[0];
	BOOST_CHECK(request.cancel_on_restart);
	BOOST_CHECK(request.priority == edt::build_priority::low);
	BOOST_CHECK(request.report == edt::build_report::problems_only);
	BOOST_CHECK(!request.show_progress);
	BOOST_CHECK(!request.reload_after);
	advance(5s);
	BOOST_CHECK_EQUAL(starts.size(), 1u);
}

BOOST_AUTO_TEST_CASE(when_off_changes_are_ignored)
{
	make(false);
	BOOST_CHECK(is(state(), edt::live_state::off));
	change();
	advance(1s);
	BOOST_CHECK(starts.empty());
	BOOST_CHECK(is(state(), edt::live_state::off));
}

BOOST_AUTO_TEST_CASE(irrelevant_changes_are_ignored)
{
	make();
	change(false);
	BOOST_CHECK(is(state(), edt::live_state::idle));
	advance(1s);
	BOOST_CHECK(starts.empty());
}

BOOST_AUTO_TEST_CASE(a_change_during_a_build_restarts_it_and_the_restarted_build_reloads_nothing)
{
	make();
	change();
	advance(500ms);
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);

	change();
	BOOST_CHECK(is(state(), edt::live_state::building));
	advance(500ms);
	BOOST_REQUIRE_EQUAL(starts.size(), 2u);
	BOOST_CHECK(starts[1].cancel_on_restart);

	finish(0, edt::build_outcome::restarted);
	advance(10s);
	BOOST_CHECK(reloads.empty());
	BOOST_CHECK(is(state(), edt::live_state::building));

	finish(1, edt::build_outcome::succeeded);
	live->tick(); // the last change is 10 s old: quiet already
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
}

BOOST_AUTO_TEST_CASE(success_reloads_only_after_the_quiet_period)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	live->tick();
	BOOST_CHECK(is(state(), edt::live_state::settling));
	BOOST_CHECK(live->status().last_build_end.has_value());
	BOOST_CHECK_EQUAL(live->status().job, 40u);

	advance(2499ms); // 2999 ms after the change
	BOOST_CHECK(reloads.empty());
	advance(1ms);
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
	BOOST_CHECK(is(state(), edt::live_state::reloading));
	deliver();
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
	BOOST_CHECK(live->status().last_reload == now);
	BOOST_CHECK(reload_problems().empty());
	advance(10s);
	BOOST_CHECK_EQUAL(reloads.size(), 1u);
}

BOOST_AUTO_TEST_CASE(the_quiet_period_comes_from_the_settings)
{
	settings.live_quiet_ms = 0;
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	live->tick();
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
}

BOOST_AUTO_TEST_CASE(a_change_during_a_successful_build_reloads_nothing)
{
	make();
	change();
	advance(500ms);
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	now += 100ms;
	change();
	finish(0, edt::build_outcome::succeeded);
	advance(100ms);
	BOOST_CHECK(reloads.empty());
	BOOST_CHECK(is(state(), edt::live_state::pending));
	advance(400ms);
	BOOST_CHECK_EQUAL(starts.size(), 2u);
	BOOST_CHECK(reloads.empty());
}

BOOST_AUTO_TEST_CASE(compile_errors_give_errors_and_no_reload)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::failed, 2);
	live->tick();
	BOOST_CHECK(is(state(), edt::live_state::errors));
	BOOST_CHECK_EQUAL(live->status().errors, 2);
	advance(10s);
	BOOST_CHECK(reloads.empty());
	BOOST_CHECK_EQUAL(starts.size(), 1u);
}

BOOST_AUTO_TEST_CASE(a_failed_reload_goes_to_the_problems_and_a_later_success_clears_it)
{
	make();
	const std::pair<core::reload_status, edt::problem_severity> cases[] = {
		{ core::reload_status::refused, edt::problem_severity::warning },
		{ core::reload_status::rolled_back, edt::problem_severity::error },
		{ core::reload_status::lost, edt::problem_severity::error },
	};
	for (const auto& [status, severity] : cases) {
		next_reload = { status, std::string{ "because of " } + std::string{ core::to_string(status) } };
		build_and_settle();
		BOOST_CHECK(is(state(), edt::live_state::reload_failed));
		const auto items = reload_problems();
		BOOST_REQUIRE_EQUAL(items.size(), 1u);
		BOOST_CHECK(items[0].severity == severity);
		BOOST_CHECK(items[0].message.find(next_reload.message) != std::string::npos);
		BOOST_CHECK(items[0].message.find("'game'") != std::string::npos);
		BOOST_CHECK_EQUAL(live->status().message, items[0].message);
	}
	// The fake reload is game_reloader::reload_quiet: no dialog exists here to be shown.
	next_reload = { core::reload_status::reloaded, {} };
	build_and_settle();
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
	BOOST_CHECK(reload_problems().empty());
}

BOOST_AUTO_TEST_CASE(rebuild_now_skips_the_debounce)
{
	make();
	BOOST_CHECK(!live->rebuild_now().has_value());
	BOOST_CHECK_EQUAL(starts.size(), 1u);

	finish(0, edt::build_outcome::succeeded);
	change();
	BOOST_CHECK(!live->rebuild_now().has_value());
	BOOST_CHECK_EQUAL(starts.size(), 2u);
	BOOST_CHECK(is(state(), edt::live_state::building));
}

BOOST_AUTO_TEST_CASE(rebuild_now_when_off_is_an_error)
{
	make(false);
	BOOST_CHECK(live->rebuild_now().has_value());
	BOOST_CHECK(starts.empty());
}

BOOST_AUTO_TEST_CASE(reload_now_skips_the_quiet_period_after_a_successful_build)
{
	make();
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::refused); // nothing to reload yet
	change();
	advance(500ms);
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::refused); // building
	finish(0, edt::build_outcome::succeeded);
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::requested);
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
	BOOST_CHECK(is(state(), edt::live_state::reloading));
	deliver();
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
	advance(10s);
	BOOST_CHECK_EQUAL(reloads.size(), 1u);
}

BOOST_AUTO_TEST_CASE(reload_now_again_after_its_own_reload_is_up_to_date)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	BOOST_REQUIRE(live->reload_now().kind == edt::reload_now_kind::requested);
	deliver();
	const auto again = live->reload_now();
	BOOST_CHECK(again.kind == edt::reload_now_kind::up_to_date);
	BOOST_CHECK_EQUAL(again.message, "up to date");
	BOOST_CHECK_EQUAL(reloads.size(), 1u);
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
}

BOOST_AUTO_TEST_CASE(reload_now_after_a_manual_build_reloads_the_newer_file)
{
	make();
	build_and_settle();
	BOOST_REQUIRE_EQUAL(reloads.size(), 1u);
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::up_to_date);

	current.erase("game"); // a manual build without reload left a newer build/bin file
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::requested);
	BOOST_CHECK_EQUAL(reloads.size(), 2u);
	deliver();
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
}

BOOST_AUTO_TEST_CASE(reload_now_before_any_live_build_takes_the_project_modules)
{
	project_modules = { "game", "tools" };
	current = { "tools" };
	make();
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::requested);
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
}

BOOST_AUTO_TEST_CASE(reload_now_is_refused_while_reloads_wait)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	BOOST_REQUIRE(live->reload_now().kind == edt::reload_now_kind::requested);
	const auto again = live->reload_now();
	BOOST_CHECK(again.kind == edt::reload_now_kind::refused);
	BOOST_CHECK(!again.message.empty());
	BOOST_CHECK_EQUAL(reloads.size(), 1u);
}

BOOST_AUTO_TEST_CASE(a_failed_reload_of_reload_now_shows_in_the_status_and_a_retry_is_allowed)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	next_reload = { core::reload_status::rolled_back, "init failed" };
	BOOST_REQUIRE(live->reload_now().kind == edt::reload_now_kind::requested);
	deliver();
	BOOST_CHECK(is(state(), edt::live_state::reload_failed));
	BOOST_CHECK(live->status().message.find("init failed") != std::string::npos);
	BOOST_CHECK_EQUAL(reload_problems().size(), 1u);

	next_reload = { core::reload_status::reloaded, {} };
	BOOST_CHECK(live->reload_now().kind == edt::reload_now_kind::requested);
	deliver();
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
	BOOST_CHECK(reload_problems().empty());
}

BOOST_AUTO_TEST_CASE(tick_only_requests_the_reload_and_the_result_completes_it)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	advance(3s);
	BOOST_REQUIRE_EQUAL(waiting.size(), 1u);
	BOOST_CHECK(is(state(), edt::live_state::reloading));
	advance(10s);
	BOOST_CHECK(is(state(), edt::live_state::reloading));
	BOOST_CHECK_EQUAL(reloads.size(), 1u);
	deliver();
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
}

BOOST_AUTO_TEST_CASE(a_build_that_changed_nothing_requests_no_reload)
{
	current = { "game" };
	make();
	build_and_settle();
	BOOST_CHECK(reloads.empty());
	BOOST_CHECK(is(state(), edt::live_state::reloaded));
}

BOOST_AUTO_TEST_CASE(a_change_during_reloading_starts_a_build_and_the_late_result_only_reaches_the_problems)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	advance(3s);
	BOOST_REQUIRE(is(state(), edt::live_state::reloading));

	change();
	BOOST_CHECK(is(state(), edt::live_state::pending));
	next_reload = { core::reload_status::rolled_back, "init failed" };
	deliver();
	BOOST_CHECK_EQUAL(reload_problems().size(), 1u);
	BOOST_CHECK(is(state(), edt::live_state::pending));
	advance(500ms);
	BOOST_CHECK_EQUAL(starts.size(), 2u);
	BOOST_CHECK(is(state(), edt::live_state::building));
}

BOOST_AUTO_TEST_CASE(a_reload_result_after_the_controller_went_is_harmless)
{
	make();
	change();
	advance(500ms);
	finish(0, edt::build_outcome::succeeded);
	advance(3s);
	BOOST_REQUIRE_EQUAL(waiting.size(), 1u);
	live.reset();
	next_reload = { core::reload_status::lost, "gone" };
	deliver();
	BOOST_CHECK(reload_problems().empty());
}

BOOST_AUTO_TEST_CASE(a_manual_build_keeps_the_changes_pending_until_it_ends)
{
	make();
	building = true; // the person's build
	change();
	advance(500ms);
	BOOST_CHECK(starts.empty());
	BOOST_CHECK(is(state(), edt::live_state::pending));
	advance(1s);
	BOOST_CHECK(starts.empty());

	building = false;
	next_start = edt::action_result::failure("busy", "A build is already running");
	advance(16ms);
	BOOST_CHECK_EQUAL(starts.size(), 1u);
	BOOST_CHECK(is(state(), edt::live_state::pending));

	next_start = edt::action_result::success(boost::json::object{ { "started", true }, { "job", 8 } });
	advance(16ms);
	BOOST_CHECK_EQUAL(starts.size(), 2u);
	BOOST_CHECK(is(state(), edt::live_state::building));
	BOOST_CHECK_EQUAL(warned("did not start"), 0u);
}

BOOST_AUTO_TEST_CASE(a_start_that_fails_goes_to_errors_and_warns_once)
{
	make();
	next_start = edt::action_result::failure("disabled", "Open a project first");
	change();
	advance(500ms);
	BOOST_CHECK(is(state(), edt::live_state::errors));
	BOOST_CHECK_EQUAL(live->status().message, "Open a project first");
	advance(1s);
	BOOST_CHECK_EQUAL(starts.size(), 1u); // no retry before the next change
	change();
	advance(500ms);
	BOOST_CHECK_EQUAL(starts.size(), 2u);
	BOOST_CHECK_EQUAL(warned("Open a project first"), 1u);
	BOOST_CHECK(problems.list().empty());
}

BOOST_AUTO_TEST_CASE(a_user_cancel_of_a_live_build_goes_idle)
{
	make();
	change();
	advance(500ms);
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	building = false; // job.cancel: no on_finished comes
	advance(16ms);
	BOOST_CHECK(is(state(), edt::live_state::idle));
	advance(10s);
	BOOST_CHECK(reloads.empty());
	BOOST_CHECK_EQUAL(starts.size(), 1u);
}

BOOST_AUTO_TEST_CASE(turning_off_cancels_a_live_build)
{
	make();
	change();
	advance(500ms);
	live->set_enabled(false);
	BOOST_CHECK_EQUAL(cancels, 1);
	BOOST_CHECK(is(state(), edt::live_state::off));
	finish(0, edt::build_outcome::succeeded); // late: ignored
	live->set_enabled(true);
	advance(10s);
	BOOST_CHECK(reloads.empty());
}

BOOST_AUTO_TEST_CASE(turning_off_leaves_the_persons_build_alone)
{
	make();
	building = true;
	live->set_enabled(false);
	BOOST_CHECK_EQUAL(cancels, 0);
}

BOOST_AUTO_TEST_CASE(an_outcome_after_the_controller_went_is_harmless)
{
	make();
	change();
	advance(500ms);
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	live.reset();
	edt::build_outcome outcome;
	outcome.kind = edt::build_outcome::succeeded;
	starts[0].on_finished(outcome);
}

BOOST_AUTO_TEST_CASE(states_have_names)
{
	BOOST_CHECK_EQUAL(edt::to_string(edt::live_state::off), "off");
	BOOST_CHECK_EQUAL(edt::to_string(edt::live_state::settling), "settling");
	BOOST_CHECK_EQUAL(edt::to_string(edt::live_state::reload_failed), "reload_failed");
}

BOOST_AUTO_TEST_SUITE_END()
