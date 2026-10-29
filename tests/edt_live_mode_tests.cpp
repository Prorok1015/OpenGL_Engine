#include <boost/test/unit_test.hpp>
#include "edt_live_mode.h"
#include "edt_build_settings_model.h"
#include "edt_mcp_bridge_host.h"
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_problem_list.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Live mode in the editor (#445): the code:// changes it takes, the status text, the toggle, the Build Settings window
// turning it on, and the agent tools live.* behind the live mode switch (a real mcp_bridge_host). Nothing is built.

namespace {

namespace json = boost::json;
using namespace std::chrono_literals;

res::file_change file_change(std::string tag, res::change_kind kind = res::change_kind::modified, bool directory = false)
{
	res::file_change change;
	change.tag = res::tag{ tag };
	change.kind = kind;
	change.is_directory = directory;
	return change;
}

struct live_mode_fixture
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	edt::action_registry actions{ commands };
	cmd::main_thread_queue queue{ commands, tasks };
	edt::mcp_bridge_host bridge{ commands, queue, tasks };
	edt::problem_list problems;
	edt::editor_events events;
	std::vector<cmd::registration> registrations;

	std::chrono::steady_clock::time_point now{ std::chrono::seconds(100) };
	bool building = false;
	std::vector<edt::build_request> starts;
	std::vector<std::string> reloads;
	// The modules whose running copy is the build/bin file as it is now.
	std::set<std::string> current;
	// What next_frame queued: the editor runs them in its next UI frame, the test in run_frame().
	std::vector<std::pair<std::string, std::function<void()>>> frames;
	std::string unavailable;
	std::vector<bool> stored;
	int toggled = 0;
	// game_builder::stop_background calls (never cancel: that would drop a waiting manual build).
	int stops = 0;
	std::unique_ptr<edt::live_mode> live;

	void make()
	{
		edt::live_mode_options options;
		options.build.now = [this] { return now; };
		options.build.start_build = [this](edt::build_request request) {
			starts.push_back(std::move(request));
			building = true;
			return edt::action_result::success(json::object{ { "started", true }, { "job", 7 } });
		};
		options.build.is_building = [this] { return building; };
		options.build.stop_background = [this] {
			++stops;
			building = false;
		};
		options.build.is_loaded_current = [this](const std::string& module) { return current.contains(module); };
		options.build.modules = [] { return std::vector<std::string>{ "game" }; };
		options.next_frame = [this](const std::string& id, std::function<void()> fn) { frames.emplace_back(id, std::move(fn)); };
		options.reload_module = [this](const std::string& module) {
			reloads.push_back(module);
			current.insert(module);
			return core::reload_report{ core::reload_status::reloaded, {} };
		};
		options.unavailable_reason = [this] { return unavailable; };
		options.store_enabled = [this](bool enabled) -> std::optional<std::string> {
			stored.push_back(enabled);
			return std::nullopt;
		};
		options.on_toggled = [this] { ++toggled; };
		options.events = &events;
		live = std::make_unique<edt::live_mode>(problems, actions, std::move(options));
	}

	void run_frame()
	{
		auto queued = std::move(frames);
		frames.clear();
		for (auto& [id, fn] : queued) fn();
	}

	// What game_builder does when the build of starts[index] succeeds: the build/bin file is new.
	void succeed(std::size_t index)
	{
		BOOST_REQUIRE(index < starts.size());
		BOOST_REQUIRE(starts[index].on_finished);
		edt::build_outcome outcome;
		outcome.kind = edt::build_outcome::succeeded;
		outcome.job = 7;
		outcome.modules.push_back(edt::game_module_build_result{ "game", "build/bin/game.dll", "debuggame", 7, false });
		building = false;
		current.erase("game");
		starts[index].on_finished(outcome);
	}

	std::optional<edt::menu_item_info> toggle_item() const
	{
		for (const auto& item : actions.items("Build")) {
			if (item.id == edt::live_mode::k_toggle_id) return item;
		}
		return std::nullopt;
	}

	std::size_t drain() { return tasks.drain_main(std::chrono::hours(1)); }

	std::vector<std::string> tool_names()
	{
		std::optional<std::vector<mcp::tool_info>> tools;
		bridge.list_tools([&tools](std::vector<mcp::tool_info> listed) { tools = std::move(listed); });
		drain();
		BOOST_REQUIRE(tools.has_value());
		std::vector<std::string> names;
		for (const auto& tool : *tools) names.push_back(tool.name);
		return names;
	}

	mcp::call_result call_tool(const std::string& name)
	{
		std::optional<mcp::call_result> result;
		mcp::request_context context;
		context.session_id = "session-1";
		bridge.call_tool(context, name, json::object{}, nullptr, [&result](mcp::call_result r) { result = std::move(r); });
		drain();
		BOOST_REQUIRE(result.has_value());
		return std::move(*result);
	}

	// What the editor registers for Build > Build Game: never an agent's.
	void add_build_game_module()
	{
		cmd::command_desc build;
		build.id = "build.game_module";
		build.summary = "Build Game";
		build.mutates = true;
		build.risk = cmd::danger::deny_by_default;
		build.invoke = [](const cmd::call&) { return cmd::result::success(); };
		auto added = commands.add(std::move(build));
		BOOST_REQUIRE(added.active());
		registrations.push_back(std::move(added));
	}
};

bool contains(const std::vector<std::string>& names, std::string_view name)
{
	return std::find(names.begin(), names.end(), name) != names.end();
}

}

BOOST_AUTO_TEST_SUITE(EdtLiveSourceChangesTests)

BOOST_AUTO_TEST_CASE(CodeFiles_AreRelevant_FoldersAndOtherFilesAreNot)
{
	std::vector<res::file_change> changes{
		file_change("code://src/game.cpp"),
		file_change("code://src/game.h", res::change_kind::created),
		file_change("code://CMakeLists.txt"),
		file_change("code://CMakePresets.json"),
		file_change("code://CMakeUserPresets.json"),
		file_change("code://cmake/deps.cmake", res::change_kind::deleted),
		file_change("code://src", res::change_kind::created, true),
		file_change("code://notes.txt"),
		file_change("code://readme.md"),
	};
	const auto mapped = edt::source_changes_of(changes);
	BOOST_REQUIRE_EQUAL(mapped.size(), changes.size());
	const std::vector<bool> expected{ true, true, true, true, true, true, false, false, false };
	for (std::size_t i = 0; i < mapped.size(); ++i) {
		BOOST_TEST_CONTEXT(mapped[i].path) {
			BOOST_TEST(mapped[i].relevant == expected[i]);
		}
	}
	BOOST_TEST(mapped[0].path == "code://src/game.cpp");
}

// Agents write the sources through the resource system: their changes come as `self` and must still rebuild.
BOOST_AUTO_TEST_CASE(SelfChanges_AreKept)
{
	res::file_change change = file_change("code://src/game.cpp");
	change.self = true;
	const auto mapped = edt::source_changes_of(std::span<const res::file_change>(&change, 1));
	BOOST_REQUIRE_EQUAL(mapped.size(), 1u);
	BOOST_TEST(mapped[0].relevant);
}

BOOST_AUTO_TEST_CASE(ARename_IsRelevantWhenEitherNameIsCode)
{
	res::file_change away = file_change("code://src/game.cpp.bak", res::change_kind::renamed);
	away.old_tag = res::tag{ "code://src/game.cpp" };
	res::file_change other = file_change("code://a.txt", res::change_kind::renamed);
	other.old_tag = res::tag{ "code://b.txt" };
	const std::vector<res::file_change> changes{ away, other };
	const auto mapped = edt::source_changes_of(changes);
	BOOST_REQUIRE_EQUAL(mapped.size(), 2u);
	BOOST_TEST(mapped[0].relevant);
	BOOST_TEST(!mapped[1].relevant);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtLiveStatusTextTests)

BOOST_AUTO_TEST_CASE(EveryState_HasItsText)
{
	edt::live_status status;
	const auto text = [&status](edt::live_state state, std::string_view clock = {}) {
		status.state = state;
		return edt::live_status_text(status, clock);
	};
	BOOST_TEST(text(edt::live_state::off) == "Live: off");
	BOOST_TEST(text(edt::live_state::idle) == "Live: idle");
	BOOST_TEST(text(edt::live_state::pending) == "Live: pending");
	BOOST_TEST(text(edt::live_state::building) == "Live: building...");
	BOOST_TEST(text(edt::live_state::errors) == "Live: errors");
	status.errors = 3;
	BOOST_TEST(text(edt::live_state::errors) == "Live: errors (3)");
	BOOST_TEST(text(edt::live_state::settling) == "Live: settling");
	BOOST_TEST(text(edt::live_state::reloading) == "Live: reloading");
	BOOST_TEST(text(edt::live_state::reloaded) == "Live: reloaded");
	BOOST_TEST(text(edt::live_state::reloaded, "14:05:09") == "Live: reloaded 14:05:09");
	BOOST_TEST(text(edt::live_state::reload_failed) == "Live: reload failed");
}

BOOST_AUTO_TEST_CASE(StatusJson_GivesTheTimesAsSecondsAgo)
{
	const std::chrono::steady_clock::time_point now{ std::chrono::seconds(100) };
	edt::live_status status;
	status.state = edt::live_state::reloaded;
	status.errors = 0;
	status.warnings = 2;
	status.configuration = "debuggame";
	status.job = 12;
	status.last_build_end = now - 5s;
	status.last_reload = now - 1500ms;
	const json::object out = edt::live_status_json(true, status, now);
	BOOST_TEST(out.at("enabled").as_bool());
	BOOST_TEST(out.at("state").as_string() == "reloaded");
	BOOST_TEST(out.at("warnings").to_number<int>() == 2);
	BOOST_TEST(out.at("configuration").as_string() == "debuggame");
	BOOST_TEST(out.at("job").to_number<std::uint64_t>() == 12u);
	BOOST_TEST(out.at("last_build_end_seconds_ago").as_double() == 5.0);
	BOOST_TEST(out.at("last_reload_seconds_ago").as_double() == 1.5);

	const json::object idle = edt::live_status_json(false, edt::live_status{}, now);
	BOOST_TEST(!idle.contains("last_build_end_seconds_ago"));
	BOOST_TEST(!idle.contains("last_reload_seconds_ago"));
	BOOST_TEST(idle.at("state").as_string() == "off");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtLiveModeTests, live_mode_fixture)

BOOST_AUTO_TEST_CASE(IsOffByDefault_TheToggleTurnsItOnAndStoresIt)
{
	make();
	BOOST_TEST(!live->enabled());
	auto item = toggle_item();
	BOOST_REQUIRE(item.has_value());
	BOOST_TEST(item->label == "Live Mode");
	BOOST_TEST(item->enabled);
	BOOST_TEST(!item->checked);
	BOOST_TEST(!actions.find(edt::live_mode::k_toggle_id)->expose_as_tool);

	BOOST_REQUIRE(actions.invoke(edt::live_mode::k_toggle_id, json::object{}).ok);
	BOOST_TEST(live->enabled());
	BOOST_TEST((live->status().state == edt::live_state::idle));
	BOOST_TEST((stored == std::vector<bool>{ true }));
	BOOST_TEST(toggled == 1);
	BOOST_TEST(toggle_item()->checked);

	BOOST_REQUIRE(actions.invoke(edt::live_mode::k_toggle_id, json::object{}).ok);
	BOOST_TEST(!live->enabled());
	BOOST_TEST((stored == std::vector<bool>{ true, false }));
	BOOST_TEST(toggled == 2);
}

BOOST_AUTO_TEST_CASE(Unavailable_TheToggleIsDisabledWithTheReason_ButTurningOffStillWorks)
{
	unavailable = "The project has no code folder (code://)";
	make();
	auto item = toggle_item();
	BOOST_REQUIRE(item.has_value());
	BOOST_TEST(!item->enabled);
	BOOST_TEST(item->disabled_tooltip == unavailable);
	BOOST_TEST(!actions.invoke(edt::live_mode::k_toggle_id, json::object{}).ok);
	BOOST_TEST((live->set_enabled(true, false) == std::optional<std::string>{ unavailable }));
	BOOST_TEST(!live->enabled());

	unavailable.clear();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	unavailable = "Open a project first";
	BOOST_TEST(toggle_item()->enabled);
	BOOST_REQUIRE(actions.invoke(edt::live_mode::k_toggle_id, json::object{}).ok);
	BOOST_TEST(!live->enabled());
}

BOOST_AUTO_TEST_CASE(CodeChanges_StartALiveBuildAfterTheDebounce)
{
	make();
	live->on_changes(std::vector<res::file_change>{ file_change("code://src/game.cpp") });
	now += edt::live_build::k_debounce;
	live->tick();
	BOOST_TEST(starts.empty());

	BOOST_REQUIRE(!live->set_enabled(true, false));
	live->on_changes(std::vector<res::file_change>{ file_change("code://notes.txt") });
	now += edt::live_build::k_debounce;
	live->tick();
	BOOST_TEST(starts.empty());

	live->on_changes(std::vector<res::file_change>{ file_change("code://src/game.cpp") });
	now += edt::live_build::k_debounce;
	live->tick();
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	BOOST_TEST((live->status().state == edt::live_state::building));
}

// The reload after the quiet period is only asked for in tick(); it runs in the next UI frame.
BOOST_AUTO_TEST_CASE(TheLiveReload_NeverRunsInsideTick_OnlyInTheNextUiFrame)
{
	make();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	live->on_changes(std::vector<res::file_change>{ file_change("code://src/game.cpp") });
	now += edt::live_build::k_debounce;
	live->tick();
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	succeed(0);
	now += std::chrono::milliseconds(edt::k_default_live_quiet_ms);
	live->tick();
	live->tick();
	BOOST_TEST(reloads.empty());
	BOOST_TEST((live->status().state == edt::live_state::reloading));
	BOOST_REQUIRE_EQUAL(frames.size(), 1u);

	run_frame();
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
	BOOST_TEST((live->status().state == edt::live_state::reloaded));
}

// A successful build of the person's clears live mode's errors; live mode's own builds (background) do not count.
BOOST_AUTO_TEST_CASE(AManualBuildThatSucceeds_ClearsTheLiveErrors)
{
	make();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	BOOST_REQUIRE(!live->build().rebuild_now().has_value());
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	edt::build_outcome failed;
	failed.errors = 3;
	failed.message = "The game build failed with 3 errors (see Problems)";
	building = false;
	starts[0].on_finished(failed);
	live->tick();
	BOOST_REQUIRE((live->status().state == edt::live_state::errors));

	edt::game_module_build_result built{ "game", "build/bin/game.dll", "debuggame", 9, false, true };
	events.game_module_built.emit(built);
	BOOST_TEST((live->status().state == edt::live_state::errors));

	built.background = false;
	events.game_module_built.emit(built);
	BOOST_TEST((live->status().state == edt::live_state::idle));
	BOOST_TEST(live->status().errors == 0);
}

// "Turn off" stops the live build through stop_background, so a manual build waiting for it is kept by the builder.
BOOST_AUTO_TEST_CASE(TurningOffDuringALiveBuild_StopsItThroughStopBackground)
{
	make();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	BOOST_REQUIRE(!live->build().rebuild_now().has_value());
	BOOST_REQUIRE(building);
	BOOST_REQUIRE(actions.invoke(edt::live_mode::k_toggle_id, json::object{}).ok);
	BOOST_TEST(!live->enabled());
	BOOST_TEST(stops == 1);
}

// Reload or a define removal stops the live build (editor_system::stop_live_build): the outcome comes as `preempted`
// and live mode builds the same changes again.
BOOST_AUTO_TEST_CASE(ALiveBuildStoppedByTheEditor_IsBuiltAgain)
{
	make();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	live->on_changes(std::vector<res::file_change>{ file_change("code://src/game.cpp") });
	now += edt::live_build::k_debounce;
	live->tick();
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);

	edt::build_outcome stopped;
	stopped.kind = edt::build_outcome::preempted;
	building = false;
	starts[0].on_finished(stopped);
	now += std::chrono::milliseconds(16);
	live->tick();
	BOOST_TEST(starts.size() == 1u); // the frame of its end
	now += std::chrono::milliseconds(16);
	live->tick();
	BOOST_TEST(starts.size() == 2u);
	BOOST_TEST((live->status().state == edt::live_state::building));
}

// #466's policy: the three agent tools are the `live` category (denied while live mode is off), the switch is never an
// agent's.
BOOST_AUTO_TEST_CASE(TheCommandsHaveTheirAgentCategories)
{
	make();
	for (const char* id : { "live.status", "live.rebuild_now", "live.reload_now" }) {
		BOOST_TEST_CONTEXT(id) {
			BOOST_REQUIRE(commands.find(id) != nullptr);
			BOOST_TEST((commands.find(id)->category == cmd::agent_category::live));
		}
	}
	BOOST_REQUIRE(commands.find(edt::live_mode::k_toggle_id) != nullptr);
	BOOST_TEST((commands.find(edt::live_mode::k_toggle_id)->category == cmd::agent_category::not_for_agents));
}

BOOST_AUTO_TEST_CASE(AFrameQueuedBeforeLiveModeWasDestroyed_ReloadsNothing)
{
	make();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	BOOST_REQUIRE(live->build().reload_now().kind == edt::reload_now_kind::requested);
	BOOST_REQUIRE_EQUAL(frames.size(), 1u);
	live.reset();
	run_frame();
	BOOST_TEST(reloads.empty());
}

// The Build Settings window applies build.json; live mode follows its live_enabled through the model's callback.
BOOST_AUTO_TEST_CASE(BuildSettingsApply_TurnsLiveModeOnAndOff)
{
	make();
	test_util::temp_dir dir{ "edt_live_mode_settings" };
	dir.write("code/CMakePresets.json", R"({
		"version": 3,
		"configurePresets": [
			{ "name": "base", "hidden": true, "generator": "Ninja", "binaryDir": "${sourceDir}/../build/${presetName}" },
			{ "name": "debuggame", "displayName": "DebugGame", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "DebugGame" } }
		],
		"buildPresets": [ { "name": "debuggame", "configurePreset": "debuggame" } ]
	})");
	edt::build_settings_model model;
	model.set_on_settings_applied([this](const edt::build_settings& settings) { live->follow_settings(settings); });
	model.load(dir.path());
	BOOST_REQUIRE(model.available());

	model.set_live_enabled(true);
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST(live->enabled());
	BOOST_TEST(toggled == 1);
	// Already in build.json: nothing is stored again.
	BOOST_TEST(stored.empty());

	model.set_live_quiet_ms(1000);
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST(live->enabled());
	BOOST_TEST(toggled == 1);

	model.set_live_enabled(false);
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST(!live->enabled());
	BOOST_TEST(toggled == 2);
}

BOOST_AUTO_TEST_CASE(LiveOff_TheLiveToolsAreHiddenAndRefused)
{
	make();
	add_build_game_module();
	const auto names = tool_names();
	for (const char* name : { "live_status", "live_rebuild_now", "live_reload_now", "build_game_module", "build_live_toggle" }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(!contains(names, name));
		}
	}
	for (const char* name : { "live_status", "live_rebuild_now", "live_reload_now" }) {
		BOOST_TEST_CONTEXT(name) {
			const mcp::call_result result = call_tool(name);
			BOOST_TEST(result.is_error);
		}
	}
	BOOST_TEST(starts.empty());
}

BOOST_AUTO_TEST_CASE(LiveOn_TheLiveToolsAreListedAndCallable_BuildGameModuleStaysHidden)
{
	make();
	add_build_game_module();
	BOOST_REQUIRE(!live->set_enabled(true, false));
	const auto names = tool_names();
	for (const char* name : { "live_status", "live_rebuild_now", "live_reload_now" }) {
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(contains(names, name));
		}
	}
	BOOST_TEST(!contains(names, "build_game_module"));
	BOOST_TEST(!contains(names, "build_live_toggle"));

	mcp::call_result status = call_tool("live_status");
	BOOST_REQUIRE(!status.is_error);
	BOOST_TEST(status.structured.as_object().at("enabled").as_bool());
	BOOST_TEST(status.structured.as_object().at("state").as_string() == "idle");

	// The running copy is the build's file already: nothing to swap.
	current.insert("game");
	mcp::call_result reload = call_tool("live_reload_now");
	BOOST_REQUIRE(!reload.is_error);
	BOOST_TEST(reload.structured.as_object().at("status").as_string() == "up_to_date");
	BOOST_TEST(reload.structured.as_object().at("message").as_string() == "up to date");
	BOOST_TEST(frames.empty());

	mcp::call_result rebuild = call_tool("live_rebuild_now");
	BOOST_REQUIRE(!rebuild.is_error);
	BOOST_TEST(rebuild.structured.as_object().at("started").as_bool());
	BOOST_REQUIRE_EQUAL(starts.size(), 1u);
	// Refused while the build runs.
	BOOST_TEST(call_tool("live_reload_now").is_error);

	// The build succeeds; the agent asks for the reload without waiting for the quiet period. No tick in between: it
	// would request the reload by itself (no change, so no quiet period).
	succeed(0);
	reload = call_tool("live_reload_now");
	BOOST_REQUIRE(!reload.is_error);
	BOOST_TEST(reload.structured.as_object().at("status").as_string() == "requested");
	// Asked for on the next UI frame, never inside the call.
	BOOST_TEST(reloads.empty());
	BOOST_REQUIRE_EQUAL(frames.size(), 1u);
	BOOST_TEST(frames[0].first == "edt/live_reload_game_module/game");
	status = call_tool("live_status");
	BOOST_TEST(status.structured.as_object().at("state").as_string() == "reloading");
	// A second request waits for the first.
	BOOST_TEST(call_tool("live_reload_now").is_error);

	run_frame();
	BOOST_TEST((reloads == std::vector<std::string>{ "game" }));
	status = call_tool("live_status");
	BOOST_REQUIRE(!status.is_error);
	BOOST_TEST(status.structured.as_object().at("state").as_string() == "reloaded");
	BOOST_TEST(status.structured.as_object().contains("last_reload_seconds_ago"));

	BOOST_REQUIRE(!live->set_enabled(false, false));
	BOOST_TEST(!contains(tool_names(), "live_status"));
	BOOST_TEST(call_tool("live_status").is_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAgentWhenEnabledTests)

BOOST_AUTO_TEST_CASE(AMutatingCommand_IsAllowedOnlyWhileEnabled_AndOnlyWhenListed)
{
	bool on = false;
	cmd::command_desc desc;
	desc.id = "live.rebuild_now";
	desc.mutates = true;
	desc.agent_when_enabled = true;
	BOOST_TEST(!edt::agent_may_use(desc)); // no `enabled` at all: closed
	desc.enabled = [&on] { return on; };
	BOOST_TEST(!edt::agent_may_use(desc));
	on = true;
	BOOST_TEST(edt::agent_may_use(desc));

	desc.risk = cmd::danger::confirm;
	BOOST_TEST(!edt::agent_may_use(desc));
	desc.risk = cmd::danger::none;
	desc.long_running = true;
	BOOST_TEST(!edt::agent_may_use(desc));
	desc.long_running = false;
	desc.expose_as_tool = false;
	BOOST_TEST(!edt::agent_may_use(desc));
	desc.expose_as_tool = true;

	// The flag alone opens nothing: build.game_module stays closed even if someone marks it.
	desc.id = "build.game_module";
	BOOST_TEST(!edt::agent_may_use(desc));
	desc.id = "file.save";
	BOOST_TEST(!edt::agent_may_use(desc));
}

BOOST_AUTO_TEST_SUITE_END()
