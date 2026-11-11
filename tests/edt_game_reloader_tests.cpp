#include <boost/test/unit_test.hpp>
#include "edt_game_reloader.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Build > Reload Game Module and the reload after a build (#435): game_reloader over a fake host.

namespace {

struct log_capture
{
	log_capture()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level, std::string_view message) {
			lines.push_back(std::format("{}: {}", category, message));
		});
	}

	std::size_t count(std::string_view part) const
	{
		std::size_t n = 0;
		for (const auto& line : lines) {
			if (line.find(part) != std::string::npos) ++n;
		}
		return n;
	}

	std::vector<std::string> lines;
	engine::log_sink_handle sink; // last: removed first
};

struct reloader_fixture
{
	reloader_fixture()
	{
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	}

	edt::game_reloader_options options()
	{
		edt::game_reloader_options out;
		out.modules = [this] { return modules; };
		out.build_output = [this](const std::string& module) -> std::optional<std::filesystem::path> {
			if (!has_build) return std::nullopt;
			return std::filesystem::path{ "build/bin" } / (module + ".dll");
		};
		out.reload = [this](const std::string& module) {
			reloaded.push_back(module);
			return next;
		};
		out.building = [this] { return building; };
		out.events = &events;
		out.next_frame = [this](const std::string& id, std::function<void()> fn) { scheduled.emplace_back(id, std::move(fn)); };
		return out;
	}

	// With `defer` (wait_reason) and an each_frame that keeps its polls in `waiting`, as editor_layer does.
	edt::game_reloader_options waiting_options()
	{
		edt::game_reloader_options out = options();
		out.defer = [this] { return wait_reason; };
		out.each_frame = [this](const std::string& id, std::function<bool()> poll) {
			for (const auto& queued : waiting) {
				if (queued.first == id) return; // register_implicit ignores a taken id
			}
			waiting.emplace_back(id, std::move(poll));
		};
		return out;
	}

	void make() { reloader = std::make_unique<edt::game_reloader>(dialogs, actions, options()); }
	void make_waiting() { reloader = std::make_unique<edt::game_reloader>(dialogs, actions, waiting_options()); }

	// One UI frame of the each_frame polls: a poll that returns false is done.
	void run_waits()
	{
		auto polls = std::move(waiting);
		waiting.clear();
		for (auto& [id, poll] : polls) {
			if (poll()) waiting.emplace_back(id, std::move(poll));
		}
	}

	edt::action_result invoke(boost::json::object args = {})
	{
		return actions.invoke(edt::game_reloader::k_action_id, args);
	}

	// The messages shown to the person.
	std::vector<std::string> messages() const
	{
		std::vector<std::string> out;
		for (const auto& info : dialogs.list()) {
			if (info.kind == "message") out.push_back(info.text);
		}
		return out;
	}

	edt::game_module_build_result built(bool reload) const
	{
		edt::game_module_build_result result;
		result.module = "game";
		result.path = "build/bin/game.dll";
		result.configuration = "debuggame";
		result.reload = reload;
		return result;
	}

	edt::action_registry actions;
	edt::dialog_service dialogs;
	edt::editor_events events;
	std::vector<std::string> modules{ "game" };
	bool has_build = true;
	bool building = false;
	core::reload_report next{ core::reload_status::reloaded, {} };
	std::vector<std::string> reloaded;
	std::vector<std::pair<std::string, std::function<void()>>> scheduled;
	std::string wait_reason;
	std::vector<std::pair<std::string, std::function<bool()>>> waiting;
	std::unique_ptr<edt::game_reloader> reloader;
};

}

BOOST_FIXTURE_TEST_SUITE(edt_game_reloader_tests, reloader_fixture)

BOOST_AUTO_TEST_CASE(the_action_reloads_the_built_module_and_returns_the_status)
{
	make();
	BOOST_TEST(reloader->disabled_reason().empty());

	const auto outcome = invoke();
	BOOST_REQUIRE_MESSAGE(outcome.ok, outcome.error);
	BOOST_TEST(outcome.value.as_object().at("status").as_string() == "reloaded");
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(messages().empty());
}

BOOST_AUTO_TEST_CASE(without_a_build_the_action_is_disabled)
{
	has_build = false;
	make();
	BOOST_TEST(!reloader->disabled_reason().empty());
	BOOST_TEST(!invoke().ok);
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(while_a_build_runs_the_action_is_disabled)
{
	building = true;
	make();
	BOOST_TEST(reloader->disabled_reason() == "A build is running");
	BOOST_TEST(!invoke().ok);
	BOOST_TEST(reloaded.empty());
}

// #445: only the person's build greys Reload out; a live build is stopped first.
BOOST_AUTO_TEST_CASE(during_a_live_build_the_action_stays_enabled_and_stops_it_first)
{
	bool background = true;
	int cancels = 0;
	bool stops_at_once = false;
	auto opts = options();
	opts.background_building = [&background] { return background; };
	opts.stop_background = [&] {
		++cancels;
		if (stops_at_once) background = false;
	};
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));
	BOOST_TEST(reloader->disabled_reason().empty());

	// The job ends at the next pump: refused this time, nothing swapped while the linker may still write.
	const auto stopping = invoke();
	BOOST_REQUIRE(stopping.ok);
	BOOST_TEST(stopping.value.as_object().at("status").as_string() == "refused");
	BOOST_TEST(cancels == 1);
	BOOST_TEST(reloaded.empty());

	stops_at_once = true;
	const auto outcome = invoke();
	BOOST_REQUIRE(outcome.ok);
	BOOST_TEST(outcome.value.as_object().at("status").as_string() == "reloaded");
	BOOST_TEST(cancels == 2);
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });

	// Gone: nothing to cancel.
	BOOST_TEST(invoke().ok);
	BOOST_TEST(cancels == 2);
}

// The live build that the action stops is still running for a frame or two: the reload waits for it instead of being
// refused (a refused click would meet the next live build again, every time).
BOOST_AUTO_TEST_CASE(during_a_live_build_the_action_queues_the_reload_until_no_build_runs)
{
	bool background = true;
	int stops = 0;
	std::vector<std::pair<std::string, std::function<bool()>>> polls;
	auto opts = options();
	opts.background_building = [&background] { return background; };
	opts.stop_background = [&stops] { ++stops; };
	opts.each_frame = [&polls](const std::string& id, std::function<bool()> poll) {
		for (const auto& queued : polls) {
			if (queued.first == id) return; // register_implicit ignores a taken id
		}
		polls.emplace_back(id, std::move(poll));
	};
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));

	const auto queued = invoke();
	BOOST_REQUIRE_MESSAGE(queued.ok, queued.error);
	BOOST_TEST(queued.value.as_object().at("status").as_string() == "queued");
	BOOST_TEST(stops == 1);
	BOOST_TEST(reloaded.empty());
	BOOST_REQUIRE(polls.size() == 1u);
	BOOST_TEST(polls[0].first == "edt/reload_game_module/waiting/game");

	BOOST_TEST(invoke().ok); // a second click: still one reload waiting
	BOOST_TEST(polls.size() == 1u);

	BOOST_TEST(polls[0].second()); // the stopped job has not ended yet: keeps waiting
	BOOST_TEST(reloaded.empty());
	background = false;
	// A click after the job ended but before the wait ran joins it: no swap ahead of the queued one.
	const auto joined = invoke();
	BOOST_REQUIRE(joined.ok);
	BOOST_TEST(joined.value.as_object().at("status").as_string() == "queued");
	BOOST_TEST(reloaded.empty());
	BOOST_TEST(!polls[0].second());
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(messages().empty());
}

BOOST_AUTO_TEST_CASE(a_build_not_taken_is_shown_with_its_reason)
{
	make();
	for (const auto status : { core::reload_status::refused, core::reload_status::rolled_back, core::reload_status::lost }) {
		next = { status, std::string{ "because of " } + std::string{ core::to_string(status) } };
		const auto outcome = invoke();
		BOOST_REQUIRE(outcome.ok);
		BOOST_TEST(outcome.value.as_object().at("status").as_string() == core::to_string(status));
	}
	const auto shown = messages();
	BOOST_REQUIRE(shown.size() == 3u);
	BOOST_TEST(shown[0].find("because of refused") != std::string::npos);
	BOOST_TEST(shown[0].find("running version is kept") != std::string::npos);
	BOOST_TEST(shown[1].find("rolled back") != std::string::npos);
	BOOST_TEST(shown[2].find("because of lost") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_build_marked_for_reload_is_reloaded_on_the_next_frame)
{
	make();
	events.game_module_built.emit(built(true));
	BOOST_TEST(reloaded.empty()); // not inside the job pump that finished the build
	BOOST_REQUIRE(scheduled.size() == 1u);
	BOOST_TEST(scheduled.front().first == "edt/reload_game_module/game");

	scheduled.front().second();
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
}

BOOST_AUTO_TEST_CASE(a_build_not_marked_for_reload_is_left_alone)
{
	make();
	events.game_module_built.emit(built(false));
	BOOST_TEST(scheduled.empty());
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(a_reload_scheduled_before_the_reloader_went_does_nothing)
{
	make();
	events.game_module_built.emit(built(true));
	BOOST_REQUIRE(scheduled.size() == 1u);
	reloader.reset();
	scheduled.front().second();
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(reload_quiet_returns_a_build_not_taken_without_a_message)
{
	make();
	for (const auto status : { core::reload_status::refused, core::reload_status::rolled_back, core::reload_status::lost }) {
		next = { status, std::string{ "because of " } + std::string{ core::to_string(status) } };
		const auto report = reloader->reload_quiet("game");
		BOOST_TEST((report.status == status));
		BOOST_TEST(report.message == next.message);
	}
	BOOST_TEST(reloaded.size() == 3u);
	BOOST_TEST(messages().empty());
}

// #601: only a reload the person asked for may load a build the host took back; live mode's (quiet) never does.
BOOST_AUTO_TEST_CASE(only_the_persons_reload_goes_through_reload_by_person)
{
	std::vector<std::string> by_person;
	edt::game_reloader_options with_person = options();
	with_person.reload_by_person = [&](const std::string& module) {
		by_person.push_back(module);
		return next;
	};
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(with_person));

	BOOST_TEST(invoke().ok);
	BOOST_TEST(by_person == std::vector<std::string>{ "game" });
	BOOST_TEST(reloaded.empty());

	reloader->reload_quiet("game");
	BOOST_TEST(by_person.size() == 1u);
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
}

BOOST_AUTO_TEST_CASE(reload_quiet_is_refused_while_a_build_runs)
{
	make();
	building = true;
	const auto report = reloader->reload_quiet("game");
	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message == "a build is running");
	BOOST_TEST(reloaded.empty());
	BOOST_TEST(messages().empty());
}

// #600: a reload while a level or an asset loads waits for it instead of being refused, and says why once per reason.
BOOST_AUTO_TEST_CASE(the_action_waits_while_defer_names_a_reason)
{
	log_capture logs;
	wait_reason = "a level is loading";
	make_waiting();
	BOOST_TEST(reloader->disabled_reason().empty());

	const auto queued = invoke();
	BOOST_REQUIRE_MESSAGE(queued.ok, queued.error);
	BOOST_TEST(queued.value.as_object().at("status").as_string() == "queued");
	BOOST_TEST(std::string(queued.value.as_object().at("message").as_string()).find("a level is loading") != std::string::npos);
	BOOST_REQUIRE(waiting.size() == 1u);
	BOOST_TEST(waiting[0].first == "edt/reload_game_module/waiting/game");
	BOOST_TEST(invoke().ok); // a second click: still one reload waiting
	BOOST_TEST(waiting.size() == 1u);

	run_waits();
	run_waits();
	BOOST_TEST(reloaded.empty());
	BOOST_TEST(logs.count("waits: a level is loading") == 1u);
	wait_reason = "an asset is being added to the scene";
	run_waits();
	run_waits();
	BOOST_TEST(logs.count("waits: an asset is being added to the scene") == 1u);
	BOOST_TEST(reloaded.empty());

	wait_reason.clear();
	run_waits();
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(waiting.empty());
	BOOST_TEST(messages().empty());
}

BOOST_AUTO_TEST_CASE(a_build_marked_for_reload_waits_while_defer_names_a_reason)
{
	wait_reason = "a level is loading";
	make_waiting();
	events.game_module_built.emit(built(true));
	BOOST_REQUIRE(scheduled.size() == 1u);
	scheduled.front().second();
	BOOST_TEST(reloaded.empty());
	BOOST_REQUIRE(waiting.size() == 1u);
	// Not the next-frame id, which is still running the frame that queued it.
	BOOST_TEST(waiting[0].first == "edt/reload_game_module/waiting/game");

	wait_reason.clear();
	run_waits();
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(waiting.empty());
}

// Live mode (#445) waits in `reloading` for this report: it must come once the reload ran, never be dropped.
BOOST_AUTO_TEST_CASE(reload_quiet_when_ready_hands_over_its_report_once_the_reload_ran)
{
	wait_reason = "an asset is being added to the scene";
	make_waiting();
	next = { core::reload_status::rolled_back, "the new build did not load" };
	std::vector<core::reload_status> reports;
	const auto record = [&reports](const core::reload_report& report) { reports.push_back(report.status); };

	reloader->reload_quiet_when_ready("game", record);
	BOOST_TEST(reports.empty());
	BOOST_REQUIRE(waiting.size() == 1u);
	BOOST_TEST(waiting[0].first == "edt/reload_game_module/waiting/game");

	wait_reason.clear();
	building = true; // a build that starts meanwhile holds it too
	run_waits();
	BOOST_TEST(reports.empty());
	building = false;
	run_waits();
	BOOST_REQUIRE(reports.size() == 1u);
	BOOST_TEST((reports[0] == core::reload_status::rolled_back));
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(messages().empty());

	reloader->reload_quiet_when_ready("game", record); // nothing holds it: now
	BOOST_TEST(reports.size() == 2u);
	BOOST_TEST(waiting.empty());
}

// The person's Reload, live mode's and the one after a build meet in one wait: one swap (in PR 2, one Play snapshot),
// every caller gets its report.
BOOST_AUTO_TEST_CASE(requests_for_a_module_that_waits_join_one_wait_and_one_swap)
{
	wait_reason = "a level is loading";
	make_waiting();
	int live_reports = 0;
	int late_reports = 0;
	BOOST_TEST(invoke().ok);
	reloader->reload_quiet_when_ready("game", [&live_reports](const core::reload_report&) { ++live_reports; });
	events.game_module_built.emit(built(true));
	BOOST_REQUIRE(scheduled.size() == 1u);
	scheduled.front().second();
	BOOST_REQUIRE(waiting.size() == 1u);

	wait_reason.clear();
	// Nothing holds it any more, but the wait has not run: a request now joins it instead of swapping first.
	reloader->reload_quiet_when_ready("game", [&late_reports](const core::reload_report&) { ++late_reports; });
	BOOST_TEST(reloaded.empty());
	run_waits();
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(live_reports == 1);
	BOOST_TEST(late_reports == 1);
	BOOST_TEST(waiting.empty());
}

// A build not taken is shown when one of the joined requests was the person's, and still handed to the quiet one.
BOOST_AUTO_TEST_CASE(a_joined_wait_shows_the_message_when_any_request_was_not_quiet)
{
	wait_reason = "a level is loading";
	make_waiting();
	next = { core::reload_status::refused, "the copy does not match" };
	std::vector<core::reload_status> reports;
	reloader->reload_quiet_when_ready("game", [&reports](const core::reload_report& report) { reports.push_back(report.status); });
	BOOST_TEST(invoke().ok);
	wait_reason.clear();
	run_waits();
	BOOST_REQUIRE(reports.size() == 1u);
	BOOST_TEST((reports[0] == core::reload_status::refused));
	BOOST_TEST(messages().size() == 1u);
}

BOOST_AUTO_TEST_CASE(reload_quiet_is_refused_while_defer_names_a_reason)
{
	wait_reason = "a level is loading";
	make_waiting();
	const auto report = reloader->reload_quiet("game");
	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("a level is loading") != std::string::npos);
	BOOST_TEST(reloaded.empty());
	BOOST_TEST(waiting.empty());
}

// HOT_RELOAD_PLAY.md 3.1: Play takes its snapshot after every check and right before the swap.
BOOST_AUTO_TEST_CASE(before_reload_runs_right_before_the_swap_and_only_then)
{
	std::vector<std::string> order;
	std::string restart;
	auto opts = options();
	opts.reload = [this, &order](const std::string& module) {
		order.push_back("swap " + module);
		return next;
	};
	opts.before_reload = [&order, &restart](const std::string& module) {
		order.push_back("before " + module);
		return restart;
	};
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));

	BOOST_TEST(invoke().ok);
	BOOST_TEST(order == (std::vector<std::string>{ "before game", "swap game" }));

	order.clear();
	building = true;
	BOOST_TEST((reloader->reload_quiet("game").status == core::reload_status::refused));
	building = false;
	has_build = false;
	BOOST_TEST((reloader->reload_quiet("game").status == core::reload_status::refused));
	has_build = true;
	BOOST_TEST(order.empty());

	log_capture logs;
	restart = "an engine component has no schema";
	BOOST_TEST((reloader->reload_quiet("game").status == core::reload_status::reloaded));
	BOOST_TEST(order == (std::vector<std::string>{ "before game", "swap game" }));
	BOOST_TEST(logs.count("an engine component has no schema. Play restarts") == 1u);
}

// Editor modules (#664): named in `modules` after the runtime ones, reloaded on their own, never with a Play snapshot.
BOOST_AUTO_TEST_CASE(an_editor_module_is_reloaded_by_name_without_a_play_snapshot)
{
	modules = { "game", "game_editor" };
	std::vector<std::string> before;
	auto opts = options();
	opts.is_editor_module = [](const std::string& module) { return module == "game_editor"; };
	opts.before_reload = [&before](const std::string& module) {
		before.push_back(module);
		return std::string{};
	};
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));

	const auto outcome = invoke({ { "module", "game_editor" } });
	BOOST_REQUIRE_MESSAGE(outcome.ok, outcome.error);
	BOOST_TEST(outcome.value.as_object().at("status").as_string() == "reloaded");
	BOOST_TEST(reloaded == std::vector<std::string>{ "game_editor" });
	BOOST_TEST(before.empty());

	reloaded.clear();
	BOOST_TEST(invoke().ok);
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" }); // without a name: the runtime module comes first
	BOOST_TEST((before == std::vector<std::string>{ "game" }));
}

BOOST_AUTO_TEST_CASE(an_editor_module_that_is_the_build_in_use_is_not_reloaded_again)
{
	modules = { "game", "game_editor" };
	bool current = true;
	auto opts = options();
	opts.is_editor_module = [](const std::string& module) { return module == "game_editor"; };
	opts.is_current = [&current](const std::string&) { return current; };
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));

	BOOST_TEST((reloader->reload_quiet("game_editor").status == core::reload_status::reloaded));
	BOOST_TEST(reloaded.empty());
	BOOST_TEST(messages().empty());

	// The game module is reloaded when asked, current or not (a person may want it restarted).
	BOOST_TEST((reloader->reload_quiet("game").status == core::reload_status::reloaded));
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });

	current = false;
	reloaded.clear();
	BOOST_TEST((reloader->reload_quiet("game_editor").status == core::reload_status::reloaded));
	BOOST_TEST(reloaded == std::vector<std::string>{ "game_editor" });
}

BOOST_AUTO_TEST_CASE(a_build_of_an_editor_module_marked_for_reload_is_reloaded_on_the_next_frame)
{
	modules = { "game", "game_editor" };
	auto opts = options();
	opts.is_editor_module = [](const std::string& module) { return module == "game_editor"; };
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));

	auto result = built(true);
	result.module = "game_editor";
	result.path = "build/bin/game_editor.dll";
	events.game_module_built.emit(result);

	BOOST_REQUIRE_EQUAL(scheduled.size(), 1u);
	BOOST_TEST(scheduled[0].first == std::string(edt::game_reloader::k_reload_frame_id) + "game_editor");
	scheduled[0].second();
	BOOST_TEST(reloaded == std::vector<std::string>{ "game_editor" });
}

BOOST_AUTO_TEST_CASE(an_editor_module_that_was_not_taken_is_called_an_editor_module_in_the_message)
{
	modules = { "game", "game_editor" };
	auto opts = options();
	opts.is_editor_module = [](const std::string& module) { return module == "game_editor"; };
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));
	next = { core::reload_status::rolled_back, "it failed to register" };

	BOOST_TEST((reloader->reload("game_editor").status == core::reload_status::rolled_back));
	const auto shown = messages();
	BOOST_REQUIRE_EQUAL(shown.size(), 1u);
	BOOST_TEST(shown[0].find("The editor module 'game_editor'") != std::string::npos);
	BOOST_TEST(shown[0].find("it failed to register") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(only_an_editor_module_that_has_a_build_is_taken_when_no_runtime_module_is_built)
{
	modules = { "game", "game_editor" };
	auto opts = options();
	opts.is_editor_module = [](const std::string& module) { return module == "game_editor"; };
	opts.build_output = [](const std::string& module) -> std::optional<std::filesystem::path> {
		if (module == "game") return std::nullopt;
		return std::filesystem::path{ "build/bin" } / (module + ".dll");
	};
	reloader = std::make_unique<edt::game_reloader>(dialogs, actions, std::move(opts));

	BOOST_TEST(invoke().ok);
	BOOST_TEST(reloaded == std::vector<std::string>{ "game_editor" });
}

BOOST_AUTO_TEST_SUITE_END()
