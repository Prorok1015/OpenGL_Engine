#include <boost/test/unit_test.hpp>
#include "edt_build_settings_model.h"
#include "edt_agent_confirmations.h"
#include "edt_mcp_bridge_host.h"
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <snk/project/prj_cmake_presets.h>
#include <snk/project/prj_json_text.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The model behind the Build Settings window (#444): what it reads from the presets and build.json, the edits it
// queues and what Apply writes; preset_for_configuration; build.settings.get through the MCP bridge.

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

// The template's presets (Editor/data/templates/projects/empty/code/CMakePresets.json).
constexpr std::string_view k_main = R"({
    "version": 3,
    "configurePresets": [
        {
            "name": "base",
            "hidden": true,
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/../build/${presetName}",
            "cacheVariables": { "CMAKE_EXPORT_COMPILE_COMMANDS": "ON" }
        },
        { "name": "debuggame", "displayName": "DebugGame", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "DebugGame" } },
        { "name": "release", "displayName": "Release", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } }
    ],
    "buildPresets": [
        { "name": "debuggame", "configurePreset": "debuggame" },
        { "name": "release", "configurePreset": "release" }
    ]
}
)";

constexpr std::string_view k_user = R"({
    "$comment": "mine",
    "version": 3,
    "vendor": { "example.com/tool": { "x": 1 } },
    "configurePresets": [
        {
            "name": "user-debuggame",
            "inherits": "debuggame",
            "binaryDir": "${sourceDir}/../build/debuggame",
            "cacheVariables": { "MY_FLAG": "1", "OTHER": "a" }
        }
    ],
    "buildPresets": [
        { "name": "user-debuggame", "inherits": "debuggame", "configurePreset": "user-debuggame", "jobs": 6 }
    ]
}
)";

std::string read_text(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

json::value read_json(const fs::path& file)
{
	return json::parse(read_text(file));
}

struct model_fixture
{
	test_util::temp_dir dir{ "edt_build_settings_model" };
	edt::build_settings_model model;

	model_fixture()
	{
		dir.write("code/CMakePresets.json", k_main);
		model.set_estimate(edt::jobs_estimate{ 4, 8, 16 * edt::k_gib, edt::compiler_family::gnu });
	}

	const fs::path& root() const { return dir.path(); }
	fs::path user_file() const { return root() / "code" / "CMakeUserPresets.json"; }
	fs::path settings_file() const { return root() / ".snk" / "build.json"; }

	void with_user() { dir.write("code/CMakeUserPresets.json", k_user); }
	void with_settings(std::string_view text) { dir.write(".snk/build.json", text); }
	void load() { model.load(root()); }

	// Writes through prj::write_text_atomically, except `failing`, which fails.
	void fail_writes_of(fs::path failing)
	{
		model.set_writer([failing = std::move(failing)](const fs::path& file, std::string_view text) -> std::optional<std::string> {
			if (file == failing) return "disk full";
			return prj::write_text_atomically(file, text);
		});
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtBuildSettingsModelTests, model_fixture)

BOOST_AUTO_TEST_CASE(Load_TeamDefinesAreReadOnly_UserDefinesAndJobsFromTheUserPresets)
{
	with_user();
	with_settings(R"({"configuration":"debuggame","jobs_mode":"manual","reload_after_build":false})");
	load();
	BOOST_REQUIRE_MESSAGE(model.available(), model.unavailable_reason());
	BOOST_TEST(model.load_errors().empty());

	const auto& configurations = model.configurations();
	BOOST_REQUIRE_EQUAL(configurations.size(), 2u);
	BOOST_TEST(configurations[0].name == "debuggame");
	BOOST_TEST(configurations[0].display_name == "DebugGame");
	BOOST_TEST(configurations[1].display_name == "Release");
	BOOST_TEST(configurations[0].generator == "Ninja");
	BOOST_TEST(configurations[0].team_defines.at("CMAKE_BUILD_TYPE") == "DebugGame");
	BOOST_TEST(configurations[0].team_defines.at("CMAKE_EXPORT_COMPILE_COMMANDS") == "ON");
	BOOST_TEST(configurations[0].user_defines.size() == 2u);
	BOOST_TEST(configurations[0].user_defines.at("MY_FLAG") == "1");
	BOOST_TEST(configurations[1].user_defines.empty());

	BOOST_TEST(model.selected_configuration() == "debuggame");
	BOOST_TEST((model.settings().jobs_mode == edt::jobs_mode::manual_jobs));
	BOOST_TEST(!model.settings().reload_after_build);
	BOOST_TEST((model.preset_jobs() == std::optional<int>(6)));
	BOOST_TEST(model.manual_jobs() == 6);
	BOOST_TEST(model.preset_for("debuggame") == "user-debuggame");
	BOOST_TEST(model.preset_for("release") == "release");
	BOOST_TEST(!model.dirty());
	BOOST_TEST(model.pending_files().empty());
	BOOST_TEST(model.changed_on_disk().empty());
}

BOOST_AUTO_TEST_CASE(Load_WithoutAProjectOrPresets_IsUnavailable)
{
	model.load({});
	BOOST_TEST(model.unavailable_reason() == "Open a project first");
	fs::remove(root() / "code" / "CMakePresets.json");
	load();
	BOOST_TEST(model.unavailable_reason() == "The project has no code/CMakePresets.json");
	BOOST_TEST(model.configurations().empty());
}

BOOST_AUTO_TEST_CASE(ManualJobs_WithoutAPresetValue_StartAtTheEstimate_AndAreClamped)
{
	load();
	BOOST_TEST(model.manual_jobs() == 4);
	model.set_manual_jobs(5000);
	BOOST_TEST(model.manual_jobs() == edt::k_max_manual_jobs);
	model.set_manual_jobs(0);
	BOOST_TEST(model.manual_jobs() == 1);
}

BOOST_AUTO_TEST_CASE(SetDefine_ARefusedEditIsNotQueued)
{
	with_user();
	load();
	const auto team = model.set_define("debuggame", "CMAKE_BUILD_TYPE", "Debug");
	BOOST_REQUIRE(team.has_value());
	BOOST_TEST(team->find("read-only") != std::string::npos);
	BOOST_TEST(model.set_define("debuggame", "", "x").has_value());
	BOOST_TEST(model.set_define("debuggame", "BAD", "${nope}").has_value());
	BOOST_TEST(model.set_define("nope", "A", "1").has_value());
	BOOST_TEST(!model.dirty());
	BOOST_TEST(model.pending_user_edits().empty());
	BOOST_TEST(model.find("debuggame")->user_defines.size() == 2u);

	BOOST_TEST(!model.set_define("debuggame", "NEW", "x").has_value());
	BOOST_TEST(model.dirty());
	BOOST_TEST(model.find("debuggame")->user_defines.at("NEW") == "x");
}

BOOST_AUTO_TEST_CASE(SetDefineEverywhere_AllOrNone)
{
	load();
	BOOST_TEST(model.set_define_everywhere("CMAKE_BUILD_TYPE", "Debug").has_value());
	BOOST_TEST(model.pending_user_edits().empty());
	BOOST_TEST(!model.set_define_everywhere("SNK_X", "ON").has_value());
	BOOST_TEST(model.pending_user_edits().size() == 2u);
	BOOST_TEST(model.find("debuggame")->user_defines.at("SNK_X") == "ON");
	BOOST_TEST(model.find("release")->user_defines.at("SNK_X") == "ON");
}

BOOST_AUTO_TEST_CASE(Apply_WritesBothFiles_AndKeepsUnknownFields)
{
	with_user();
	with_settings(R"({"unknown_key":5,"jobs_mode":"auto"})");
	load();
	BOOST_REQUIRE(!model.set_define("release", "NEW", "x").has_value());
	model.set_log_verbosity(edt::log_verbosity::verbose);
	BOOST_TEST((model.pending_files() == std::vector<fs::path>{ user_file(), settings_file() }));

	const auto result = model.apply();
	BOOST_REQUIRE_MESSAGE(result.ok(), result.user_presets_error + " / " + result.build_settings_error);
	BOOST_TEST(result.fresh.empty());
	BOOST_TEST(!model.dirty());
	BOOST_TEST(model.find("release")->user_defines.at("NEW") == "x");
	BOOST_TEST(model.changed_on_disk().empty());

	const json::value user = read_json(user_file());
	BOOST_TEST((user.as_object().at("$comment").as_string() == "mine"));
	BOOST_TEST(user.as_object().contains("vendor"));
	BOOST_TEST(prj::user_defines(user, "release").at("NEW") == "x");
	BOOST_TEST(prj::user_defines(user, "debuggame").at("MY_FLAG") == "1");

	const json::value settings = read_json(settings_file());
	BOOST_TEST((settings.as_object().at("unknown_key").as_int64() == 5));
	BOOST_TEST((settings.as_object().at("log_verbosity").as_string() == "verbose"));
	BOOST_TEST(!settings.as_object().contains("fresh_configure"));
}

BOOST_AUTO_TEST_CASE(Apply_CreatesTheSnkFolder)
{
	load();
	model.set_reload_after_build(false);
	BOOST_TEST((model.pending_files() == std::vector<fs::path>{ settings_file() }));
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST((read_json(settings_file()).as_object().at("reload_after_build").as_bool() == false));
	BOOST_TEST(!fs::exists(user_file()));
}

BOOST_AUTO_TEST_CASE(Live_RoundTripsThroughApply_AndRevert)
{
	with_settings(R"({"unknown_key":5})");
	load();
	model.set_live_enabled(true);
	model.set_live_quiet_ms(1500);
	model.set_live_jobs(3);
	BOOST_TEST(model.dirty());
	model.revert();
	BOOST_TEST(!model.dirty());
	BOOST_TEST(!model.settings().live_enabled);

	model.set_live_enabled(true);
	model.set_live_quiet_ms(1500);
	model.set_live_jobs(3);
	BOOST_TEST((model.pending_files() == std::vector<fs::path>{ settings_file() }));
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST(!model.dirty());
	BOOST_TEST(model.settings().live_enabled);
	BOOST_TEST(model.settings().live_quiet_ms == 1500);
	BOOST_TEST(model.settings().live_jobs == 3);
	const json::object settings = read_json(settings_file()).as_object();
	BOOST_TEST((settings.at("unknown_key").as_int64() == 5));
	BOOST_TEST(settings.at("live_enabled").as_bool());
	BOOST_TEST((settings.at("live_quiet_ms").as_int64() == 1500));
	BOOST_TEST((settings.at("live_jobs").as_int64() == 3));

	model.set_live_quiet_ms(-5);
	model.set_live_jobs(5000);
	BOOST_TEST(model.settings().live_quiet_ms == 0);
	BOOST_TEST(model.settings().live_jobs == edt::k_max_live_jobs);
	model.set_live_quiet_ms(edt::k_max_live_quiet_ms + 1);
	BOOST_TEST(model.settings().live_quiet_ms == edt::k_max_live_quiet_ms);
}

BOOST_AUTO_TEST_CASE(ManualJobs_GoToEveryConfiguration_AutoClearsThem)
{
	load();
	model.set_jobs_mode(edt::jobs_mode::manual_jobs);
	model.set_manual_jobs(3);
	BOOST_TEST(model.pending_user_edits().size() == 2u);
	BOOST_REQUIRE(model.apply().ok());
	json::value user = read_json(user_file());
	BOOST_TEST((prj::user_jobs(user, "debuggame") == std::optional<int>(3)));
	BOOST_TEST((prj::user_jobs(user, "release") == std::optional<int>(3)));
	BOOST_TEST(model.manual_jobs() == 3);

	model.set_jobs_mode(edt::jobs_mode::auto_jobs);
	BOOST_REQUIRE(model.apply().ok());
	user = read_json(user_file());
	BOOST_TEST(!prj::user_jobs(user, "debuggame").has_value());
	BOOST_TEST(!prj::user_jobs(user, "release").has_value());
	BOOST_TEST(edt::jobs_mode_name(model.settings().jobs_mode) == "auto");
}

BOOST_AUTO_TEST_CASE(Revert_DropsTheQueuedEdits)
{
	with_user();
	load();
	BOOST_REQUIRE(!model.set_define("debuggame", "NEW", "x").has_value());
	model.remove_define("debuggame", "MY_FLAG");
	model.set_reload_after_build(false);
	model.set_jobs_mode(edt::jobs_mode::manual_jobs);
	BOOST_TEST(model.dirty());
	model.revert();
	BOOST_TEST(!model.dirty());
	BOOST_TEST(model.pending_files().empty());
	BOOST_TEST(model.find("debuggame")->user_defines.size() == 2u);
	BOOST_TEST(model.find("debuggame")->user_defines.contains("MY_FLAG"));
	BOOST_TEST(model.settings().reload_after_build);
	BOOST_TEST(!fs::exists(settings_file()));
}

BOOST_AUTO_TEST_CASE(ChangedOnDisk_IsSeen_AndApplyMergesOntoTheCurrentFile)
{
	with_settings(R"({"log_verbosity":"normal"})");
	load();
	model.set_reload_after_build(false);
	with_settings(R"({"log_verbosity":"errors","added":true})");
	BOOST_TEST((model.changed_on_disk() == std::vector<fs::path>{ settings_file() }));

	BOOST_REQUIRE(model.apply().ok());
	const json::value settings = read_json(settings_file());
	BOOST_TEST((settings.as_object().at("log_verbosity").as_string() == "errors"));
	BOOST_TEST((settings.as_object().at("reload_after_build").as_bool() == false));
	BOOST_TEST(settings.as_object().at("added").as_bool());
	BOOST_TEST(model.changed_on_disk().empty());
}

BOOST_AUTO_TEST_CASE(RemovedDefine_MarksTheConfigurationForAFreshConfigure)
{
	with_user();
	load();
	model.remove_define("debuggame", "MY_FLAG");
	BOOST_TEST((model.pending_fresh() == std::vector<std::string>{ "debuggame" }));
	BOOST_TEST((model.pending_files() == std::vector<fs::path>{ user_file(), settings_file() }));

	const auto result = model.apply();
	BOOST_REQUIRE_MESSAGE(result.ok(), result.user_presets_error + " / " + result.build_settings_error);
	BOOST_TEST((result.fresh == std::vector<std::string>{ "debuggame" }));
	BOOST_TEST((model.settings().fresh_configure == std::vector<std::string>{ "debuggame" }));
	BOOST_TEST(!prj::user_defines(read_json(user_file()), "debuggame").contains("MY_FLAG"));
	BOOST_TEST((read_json(settings_file()).as_object().at("fresh_configure").as_array().size() == 1u));
	BOOST_TEST(!model.dirty());
}

// #445: every mark bumps fresh_generation, also one for a configuration that is marked already, so a build that
// started before it does not clear it.
BOOST_AUTO_TEST_CASE(RemovedDefine_BumpsTheFreshGeneration_EvenWhenMarkedAlready)
{
	with_user();
	with_settings(R"({"fresh_configure":["debuggame"],"fresh_generation":4})");
	load();
	BOOST_REQUIRE(!model.remove_define("debuggame", "MY_FLAG").has_value());
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST(model.settings().fresh_generation == 5u);
	BOOST_TEST((model.settings().fresh_configure == std::vector<std::string>{ "debuggame" }));

	// The build that started with generation 4 finishes its configure now: the newer mark stays.
	BOOST_REQUIRE(!edt::clear_fresh_configure(root() / ".snk", "debuggame", 4).has_value());
	BOOST_TEST((edt::load_build_settings(root() / ".snk").fresh_configure == std::vector<std::string>{ "debuggame" }));

	// An edit without a removal leaves the generation alone.
	model.set_reload_after_build(false);
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST(model.settings().fresh_generation == 5u);
}

BOOST_AUTO_TEST_CASE(RenamedDefine_IsARemoveAndAnAdd)
{
	with_user();
	load();
	model.remove_define("debuggame", "OTHER");
	BOOST_REQUIRE(!model.set_define("debuggame", "RENAMED", "a").has_value());
	BOOST_TEST((model.pending_fresh() == std::vector<std::string>{ "debuggame" }));
}

BOOST_AUTO_TEST_CASE(BrokenBuildJson_IsNeverOverwritten)
{
	with_user();
	constexpr std::string_view broken = "{ \"jobs_mode\": ";
	with_settings(broken);
	load();
	BOOST_TEST(!model.load_errors().empty());

	model.set_reload_after_build(false);
	auto result = model.apply();
	BOOST_TEST(!result.build_settings_error.empty());
	BOOST_TEST(read_text(settings_file()) == broken);

	// A removed define needs build.json for its --fresh: then the user presets are not written either.
	model.revert();
	const std::string user_before = read_text(user_file());
	model.remove_define("debuggame", "MY_FLAG");
	result = model.apply();
	BOOST_TEST(!result.ok());
	BOOST_TEST(!result.user_presets_error.empty());
	BOOST_TEST(read_text(user_file()) == user_before);
	BOOST_TEST(read_text(settings_file()) == broken);
	BOOST_TEST(model.dirty());
}

BOOST_AUTO_TEST_CASE(FailedBuildJsonWrite_WritesNoRemovalWithoutItsMark)
{
	with_user();
	load();
	const std::string user_before = read_text(user_file());
	BOOST_REQUIRE(!model.remove_define("debuggame", "MY_FLAG").has_value());
	fail_writes_of(settings_file());
	const auto result = model.apply();
	BOOST_TEST(result.build_settings_error == "disk full");
	BOOST_TEST(!result.user_presets_error.empty());
	BOOST_TEST(result.fresh.empty());
	BOOST_TEST(read_text(user_file()) == user_before);
	BOOST_TEST(!fs::exists(settings_file()));
	BOOST_TEST(model.dirty());
}

BOOST_AUTO_TEST_CASE(FailedUserPresetsWrite_KeepsTheFreshMark)
{
	with_user();
	load();
	const std::string user_before = read_text(user_file());
	BOOST_REQUIRE(!model.remove_define("debuggame", "MY_FLAG").has_value());
	fail_writes_of(user_file());
	const auto result = model.apply();
	BOOST_TEST(result.build_settings_error.empty());
	BOOST_TEST(result.user_presets_error == "disk full");
	BOOST_TEST((result.fresh == std::vector<std::string>{ "debuggame" }));
	BOOST_TEST(read_text(user_file()) == user_before);
	BOOST_TEST((edt::load_build_settings(root() / ".snk").fresh_configure == std::vector<std::string>{ "debuggame" }));
	BOOST_TEST(model.dirty());
}

BOOST_AUTO_TEST_CASE(WhileABuildRuns_NoDefineIsRemovedAndNoRemovalApplied)
{
	with_user();
	bool building = true;
	model.set_building([&building] { return building; });
	load();
	BOOST_TEST(model.remove_define("debuggame", "MY_FLAG").has_value());
	BOOST_TEST(!model.dirty());

	// Queued before the build started, applied while it runs: refused, since the build would clear the mark.
	building = false;
	BOOST_REQUIRE(!model.remove_define("debuggame", "MY_FLAG").has_value());
	building = true;
	BOOST_TEST(model.apply_blocked_reason().has_value());
	const std::string user_before = read_text(user_file());
	const auto refused = model.apply();
	BOOST_TEST(!refused.ok());
	BOOST_TEST(read_text(user_file()) == user_before);
	BOOST_TEST(!fs::exists(settings_file()));

	building = false;
	BOOST_TEST(!model.apply_blocked_reason().has_value());
	BOOST_TEST(model.apply().ok());
	BOOST_TEST((model.settings().fresh_configure == std::vector<std::string>{ "debuggame" }));
}

BOOST_AUTO_TEST_CASE(WhileABuildRuns_OtherEditsStillApply)
{
	with_user();
	model.set_building([] { return true; });
	load();
	BOOST_REQUIRE(!model.set_define("debuggame", "NEW", "x").has_value());
	BOOST_TEST(!model.apply_blocked_reason().has_value());
	BOOST_TEST(model.apply().ok());
}

BOOST_AUTO_TEST_CASE(AClearedFreshMark_IsTakenOverSilently)
{
	with_user();
	load();
	BOOST_REQUIRE(!model.remove_define("debuggame", "MY_FLAG").has_value());
	BOOST_REQUIRE(model.apply().ok());
	BOOST_TEST((model.settings().fresh_configure == std::vector<std::string>{ "debuggame" }));

	BOOST_REQUIRE(!edt::clear_fresh_configure(root() / ".snk", "debuggame").has_value());
	BOOST_TEST(model.refresh_fresh_configure());
	BOOST_TEST(model.changed_on_disk().empty());
	BOOST_TEST(model.settings().fresh_configure.empty());
	BOOST_TEST(!model.dirty());
}

BOOST_AUTO_TEST_CASE(AnotherChangeOnDisk_IsNotTakenOver)
{
	with_settings(R"({"fresh_configure":["debuggame"]})");
	load();
	with_settings(R"({"log_verbosity":"verbose"})");
	BOOST_TEST(!model.refresh_fresh_configure());
	BOOST_TEST((model.changed_on_disk() == std::vector<fs::path>{ settings_file() }));
	BOOST_TEST((model.settings().fresh_configure == std::vector<std::string>{ "debuggame" }));
}

BOOST_AUTO_TEST_CASE(BrokenUserPresets_AreAnErrorOnLoad_AndOnApply)
{
	dir.write("code/CMakeUserPresets.json", "[1, 2]");
	load();
	BOOST_TEST(model.available());
	BOOST_TEST(!model.load_errors().empty());
	BOOST_REQUIRE(!model.set_define("release", "A", "1").has_value());
	const auto result = model.apply();
	BOOST_TEST(!result.user_presets_error.empty());
	BOOST_TEST(read_text(user_file()) == "[1, 2]");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtPresetForConfigurationTests, model_fixture)

BOOST_AUTO_TEST_CASE(UserPreset_IsUsedWhenItExists)
{
	with_user();
	BOOST_TEST(edt::preset_for_configuration(root() / "code", "debuggame") == "user-debuggame");
	BOOST_TEST(edt::preset_for_configuration(root() / "code", "release") == "release");
}

BOOST_AUTO_TEST_CASE(NoUserFile_GivesTheTeamPreset)
{
	BOOST_TEST(edt::preset_for_configuration(root() / "code", "debuggame") == "debuggame");
}

BOOST_AUTO_TEST_CASE(BrokenUserFile_GivesTheTeamPreset)
{
	dir.write("code/CMakeUserPresets.json", "{ not json");
	BOOST_TEST(edt::preset_for_configuration(root() / "code", "debuggame") == "debuggame");
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct agent_fixture : model_fixture
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	cmd::main_thread_queue queue{ commands, tasks };
	edt::session_grants grants;
	edt::agent_confirmations confirmations{ commands, grants };
	edt::mcp_bridge_host bridge{ commands, queue, tasks, confirmations };
	std::optional<fs::path> project;
	std::vector<cmd::registration> registrations;

	agent_fixture()
	{
		project = root();
		edt::build_settings_command_options options;
		options.project_root = [this] { return project; };
		options.estimate = [] { return edt::jobs_estimate{ 4, 8, 16 * edt::k_gib, edt::compiler_family::gnu }; };
		registrations = edt::register_build_settings_commands(commands, std::move(options));
	}

	mcp::call_result call(const std::string& name)
	{
		std::optional<mcp::call_result> result;
		mcp::request_context context;
		context.session_id = "agent";
		bridge.call_tool(context, name, json::object{}, nullptr, [&result](mcp::call_result r) { result = std::move(r); });
		tasks.drain_main(std::chrono::hours(1));
		BOOST_REQUIRE(result.has_value());
		return std::move(*result);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtBuildSettingsGetTests, agent_fixture)

BOOST_AUTO_TEST_CASE(AnAgentSeesAndCallsIt)
{
	with_user();
	with_settings(R"({"jobs_mode":"manual","log_verbosity":"verbose"})");
	BOOST_REQUIRE_EQUAL(registrations.size(), 1u);

	std::optional<std::vector<mcp::tool_info>> tools;
	bridge.list_tools([&tools](std::vector<mcp::tool_info> listed) { tools = std::move(listed); });
	tasks.drain_main(std::chrono::hours(1));
	BOOST_REQUIRE(tools.has_value());
	BOOST_TEST(std::any_of(tools->begin(), tools->end(), [](const mcp::tool_info& tool) {
		return tool.name == "build_settings_get" && tool.read_only;
	}));

	const mcp::call_result result = call("build_settings_get");
	BOOST_REQUIRE(!result.is_error);
	const json::object& got = result.structured.as_object();
	BOOST_TEST((got.at("configuration").as_string() == "debuggame"));
	BOOST_TEST((got.at("jobs_mode").as_string() == "manual"));
	BOOST_TEST((got.at("log_verbosity").as_string() == "verbose"));
	BOOST_TEST(got.at("reload_after_build").as_bool());
	const json::object& jobs = got.at("jobs").as_object();
	BOOST_TEST((jobs.at("manual").as_int64() == 6));
	BOOST_TEST((jobs.at("estimate").as_object().at("jobs").as_int64() == 4));
	const json::object& configurations = got.at("configurations").as_object();
	BOOST_TEST(configurations.size() == 2u);
	const json::object& debuggame = configurations.at("debuggame").as_object();
	BOOST_TEST((debuggame.at("preset").as_string() == "user-debuggame"));
	BOOST_TEST((debuggame.at("team_defines").as_object().at("CMAKE_BUILD_TYPE").as_string() == "DebugGame"));
	BOOST_TEST((debuggame.at("user_defines").as_object().at("MY_FLAG").as_string() == "1"));
	BOOST_TEST((configurations.at("release").as_object().at("preset").as_string() == "release"));
	const json::object& live = got.at("live").as_object();
	BOOST_TEST(!live.at("enabled").as_bool());
	BOOST_TEST((live.at("quiet_ms").as_int64() == 3000));
	BOOST_TEST((live.at("jobs").as_int64() == 0));
}

BOOST_AUTO_TEST_CASE(Live_IsReported)
{
	with_settings(R"({"live_enabled":true,"live_quiet_ms":500,"live_jobs":4})");
	const mcp::call_result result = call("build_settings_get");
	BOOST_REQUIRE(!result.is_error);
	const json::object& live = result.structured.as_object().at("live").as_object();
	BOOST_TEST(live.at("enabled").as_bool());
	BOOST_TEST((live.at("quiet_ms").as_int64() == 500));
	BOOST_TEST((live.at("jobs").as_int64() == 4));
}

BOOST_AUTO_TEST_CASE(AJobsValueBelowOne_IsLeftOut)
{
	dir.write("code/CMakeUserPresets.json", R"({"version":3,"configurePresets":[{"name":"user-debuggame","inherits":"debuggame"}],
		"buildPresets":[{"name":"user-debuggame","configurePreset":"user-debuggame","jobs":0}]})");
	const mcp::call_result result = call("build_settings_get");
	BOOST_REQUIRE(!result.is_error);
	BOOST_TEST(!result.structured.as_object().at("jobs").as_object().contains("manual"));
}

BOOST_AUTO_TEST_CASE(WithoutAProject_FailsWithNoProject)
{
	project.reset();
	const mcp::call_result result = call("build_settings_get");
	BOOST_TEST(result.is_error);
	BOOST_TEST(result.content.at(0).text.rfind("no_project", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(ItIsReadOnlyAndAllowedById)
{
	const cmd::command_desc* desc = commands.find("build.settings.get");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST(!desc->mutates);
	BOOST_TEST((desc->risk == cmd::danger::none));
	BOOST_TEST(edt::agent_may_use(*desc));
	BOOST_TEST((desc->category == cmd::agent_category::read));
}

BOOST_AUTO_TEST_SUITE_END()
