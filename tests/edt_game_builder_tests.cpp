#include <boost/test/unit_test.hpp>
#include "edt_game_builder.h"
#include "edt_mcp_bridge_host.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <boost/json/object.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// game_builder (#434) on a manual task_system with a fake runner and a fake toolchain: nothing is spawned.

namespace {

namespace fs = std::filesystem;

struct recorded_call
{
	fs::path program;
	std::vector<std::string> args;
	fs::path working_dir;
	std::vector<std::string> environment;
};

struct build_fixture
{
	build_fixture()
	{
		ds::task_system_config config;
		config.mode = ds::policy::manual;
		tasks = std::make_unique<ds::task_system>(config);
		jobs = std::make_unique<edt::job_service>(*tasks);
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});

		static int counter = 0;
		const fs::path base = fs::temp_directory_path() / std::format("snk_edt_game_builder_tests_{}_{}",
			std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
		root = base / "project";
		sdk = base / "sdk";
		fs::create_directories(root / "code");
		fs::create_directories(sdk);
		std::ofstream(root / "code" / "CMakePresets.json") << "{}";
		std::ofstream(sdk / "snake_sdkConfig.cmake") << "";

		log = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (category != "editor/build") return;
			const std::lock_guard lock(log_mutex);
			log_lines.emplace_back(message);
			if (level == engine::log_level::warn) warn_lines.emplace_back(message);
		});
	}

	~build_fixture()
	{
		builder.reset();
		jobs->cancel_all_and_wait();
		std::error_code ec;
		fs::remove_all(root.parent_path(), ec);
	}

	edt::game_builder_options options()
	{
		edt::game_builder_options out;
		out.project = [this]() -> std::optional<edt::game_project> {
			if (!has_project) return std::nullopt;
			return edt::game_project{ root, { "game" } };
		};
		out.sdk_dir = sdk;
		out.engine_shared = true;
		out.find_toolchain = [this](std::stop_token) -> std::variant<edt::toolchain, std::string> {
			++toolchain_lookups;
			if (!toolchain_error.empty()) return toolchain_error;
			return edt::toolchain{ fs::path("fake-cmake"), { "PATH=/fake/bin" }, "fake" };
		};
		out.run = [this](const fs::path& program, const std::vector<std::string>& args, const platform::process_options& opts,
			const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
			calls.push_back({ program, args, opts.working_dir, opts.environment.value_or(std::vector<std::string>{}) });
			return script(calls.size() - 1, on_line, stop);
		};
		return out;
	}

	void make(edt::game_builder_options opts)
	{
		builder = std::make_unique<edt::game_builder>(*jobs, problems, dialogs, actions, std::move(opts));
	}
	void make() { make(options()); }

	edt::action_result build(boost::json::object args = {})
	{
		return actions.invoke(edt::game_builder::k_action_id, args);
	}

	void run_to_end()
	{
		while (tasks->run_one(ds::lane::io)) {}
		tasks->drain_main();
	}

	std::vector<std::string> messages() const
	{
		std::vector<std::string> out;
		for (const auto& dialog : dialogs.list()) {
			if (dialog.kind == "message") out.push_back(dialog.text);
		}
		return out;
	}

	bool warned(std::string_view text)
	{
		const std::lock_guard lock(log_mutex);
		return std::any_of(warn_lines.begin(), warn_lines.end(), [text](const std::string& line) { return line.find(text) != std::string::npos; });
	}

	bool logged(std::string_view text)
	{
		const std::lock_guard lock(log_mutex);
		return std::any_of(log_lines.begin(), log_lines.end(), [text](const std::string& line) { return line.find(text) != std::string::npos; });
	}

	std::vector<edt::problem> build_problems() const
	{
		std::vector<edt::problem> out;
		for (const auto& item : problems.list()) {
			if (item.source == "build") out.push_back(item.item);
		}
		return out;
	}

	std::unique_ptr<ds::task_system> tasks;
	std::unique_ptr<edt::job_service> jobs;
	edt::problem_list problems;
	edt::dialog_service dialogs;
	edt::action_registry actions;
	std::unique_ptr<edt::game_builder> builder;

	fs::path root;
	fs::path sdk;
	bool has_project = true;
	std::string toolchain_error;
	int toolchain_lookups = 0;
	std::vector<recorded_call> calls;
	std::function<platform::process_result(std::size_t, const std::function<void(std::string_view)>&, std::stop_token)> script =
		[](std::size_t, const std::function<void(std::string_view)>&, std::stop_token) {
			platform::process_result ok;
			ok.exit_code = 0;
			return ok;
		};

	std::mutex log_mutex;
	std::vector<std::string> log_lines;
	std::vector<std::string> warn_lines;
	engine::log_sink_handle log;
};

platform::process_result exited(int code)
{
	platform::process_result result;
	result.exit_code = code;
	return result;
}

bool has_env(const std::vector<std::string>& env, std::string_view entry)
{
	return std::find(env.begin(), env.end(), entry) != env.end();
}

}

BOOST_FIXTURE_TEST_SUITE(EdtGameBuilderTests, build_fixture)

BOOST_AUTO_TEST_CASE(Success_ConfiguresThenBuildsInCodeWithTheSdk)
{
	make();
	problems.set("build", { edt::problem{ edt::problem_severity::error, "old.cpp", 1, 0, {}, "old", {} } });
	script = [](std::size_t index, const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (index == 1) {
			on_line("[1/2] Building CXX object CMakeFiles/game.dir/src/game_module.cpp.o");
			on_line("[2/2] Linking CXX shared library ../build/bin/libgame.so");
		}
		return exited(0);
	};

	const auto started = build();
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	BOOST_CHECK(started.value.as_object().at("started").as_bool());
	BOOST_CHECK(started.value.as_object().at("job").to_number<std::uint64_t>() == builder->job());
	BOOST_CHECK(builder->is_building());
	BOOST_CHECK(build_problems().empty()); // cleared when the build starts

	run_to_end();
	BOOST_CHECK(!builder->is_building());
	BOOST_REQUIRE_EQUAL(calls.size(), 2u);
	BOOST_CHECK(calls[0].program == fs::path("fake-cmake"));
	BOOST_TEST((calls[0].args == std::vector<std::string>{ "--preset", "debuggame" }));
	BOOST_TEST((calls[1].args == std::vector<std::string>{ "--build", "--preset", "debuggame" }));
	for (const auto& call : calls) {
		BOOST_CHECK(call.working_dir == root / "code");
		const std::u8string sdk_text = sdk.u8string();
		BOOST_CHECK(has_env(call.environment, "SNAKE_SDK_DIR=" + std::string(sdk_text.begin(), sdk_text.end())));
		BOOST_CHECK(has_env(call.environment, "PATH=/fake/bin"));
	}
	BOOST_CHECK(build_problems().empty());
	BOOST_CHECK(messages().empty());
	BOOST_CHECK(logged("game module built"));
}

BOOST_AUTO_TEST_CASE(Configuration_ComesFromTheArgument)
{
	make();
	BOOST_REQUIRE(build({ { "configuration", "release" } }).ok);
	run_to_end();
	BOOST_REQUIRE_EQUAL(calls.size(), 2u);
	BOOST_TEST((calls[0].args == std::vector<std::string>{ "--preset", "release" }));
	BOOST_TEST((calls[1].args == std::vector<std::string>{ "--build", "--preset", "release" }));
	BOOST_CHECK_EQUAL(build({ { "configuration", "Debug" } }).code, "invalid_args");
}

BOOST_AUTO_TEST_CASE(CompileErrors_GoToTheProblemsWithFileAndLine)
{
	make();
	script = [](std::size_t index, const std::function<void(std::string_view)>& on_line, std::stop_token) {
		if (index == 0) return exited(0);
		on_line("[1/2] Building CXX object CMakeFiles/game.dir/src/game_module.cpp.o");
		on_line("FAILED: CMakeFiles/game.dir/src/game_module.cpp.o");
		on_line("/p/code/src/game_module.cpp:12:5: error: 'x' was not declared in this scope");
		on_line("ninja: build stopped: subcommand failed.");
		return exited(1);
	};
	BOOST_REQUIRE(build().ok);
	run_to_end();

	const auto items = build_problems();
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_CHECK(items[0].severity == edt::problem_severity::error);
	BOOST_CHECK_EQUAL(items[0].file, "/p/code/src/game_module.cpp");
	BOOST_CHECK_EQUAL(items[0].line, 12);
	const auto shown = messages();
	BOOST_REQUIRE_EQUAL(shown.size(), 1u);
	BOOST_CHECK(shown[0].find("1 error") != std::string::npos);
	BOOST_CHECK(warned("ninja: build stopped: subcommand failed."));
}

BOOST_AUTO_TEST_CASE(ConfigureFails_TheBuildStepDoesNotRun)
{
	make();
	script = [](std::size_t, const std::function<void(std::string_view)>& on_line, std::stop_token) {
		on_line("CMake Error at CMakeLists.txt:33 (find_package):");
		on_line("  Could not find a package configuration file provided by \"snake_sdk\"");
		return exited(1);
	};
	BOOST_REQUIRE(build().ok);
	run_to_end();

	BOOST_CHECK_EQUAL(calls.size(), 1u);
	BOOST_CHECK(!build_problems().empty());
	BOOST_CHECK_EQUAL(messages().size(), 1u);
}

BOOST_AUTO_TEST_CASE(NoToolchain_ShowsTheErrorAndRunsNothing)
{
	toolchain_error = "CMake was not found: install it or set build/cmake";
	make();
	BOOST_REQUIRE(build().ok);
	run_to_end();

	BOOST_CHECK(calls.empty());
	const auto shown = messages();
	BOOST_REQUIRE_EQUAL(shown.size(), 1u);
	BOOST_CHECK_EQUAL(shown[0], toolchain_error);
	BOOST_CHECK(!builder->is_building());
}

BOOST_AUTO_TEST_CASE(NoSdk_ShowsTheErrorAndLooksForNoToolchain)
{
	auto opts = options();
	opts.sdk_dir = root / "nowhere";
	make(std::move(opts));
	BOOST_REQUIRE(build().ok);
	run_to_end();

	BOOST_CHECK(calls.empty());
	BOOST_CHECK_EQUAL(toolchain_lookups, 0);
	const auto shown = messages();
	BOOST_REQUIRE_EQUAL(shown.size(), 1u);
	BOOST_CHECK(shown[0].find("snake_sdkConfig.cmake") != std::string::npos);
	BOOST_CHECK(shown[0].find("build/sdk_dir") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(NoSdkDirAtAll_NamesTheLookupOrderNotAnEmptyPath)
{
	auto opts = options();
	opts.sdk_dir.clear();
	make(std::move(opts));
	BOOST_REQUIRE(build().ok);
	run_to_end();

	const auto shown = messages();
	BOOST_REQUIRE_EQUAL(shown.size(), 1u);
	BOOST_CHECK(shown[0].find("''") == std::string::npos);
	const std::size_t cfg = shown[0].find("build/sdk_dir");
	const std::size_t installed = shown[0].find("lib/cmake/snake_sdk");
	const std::size_t tree = shown[0].find("build tree");
	BOOST_CHECK(cfg != std::string::npos && installed != std::string::npos && tree != std::string::npos);
	BOOST_CHECK(cfg < installed && installed < tree);
	BOOST_CHECK(calls.empty());
}

// It runs the project's CMake code: an MCP agent must not see or call it until the person approves (A6b).
BOOST_AUTO_TEST_CASE(AnAgentCannotStartABuild)
{
	make();
	const cmd::command_desc* command = actions.find(edt::game_builder::k_action_id);
	BOOST_REQUIRE(command != nullptr);
	BOOST_CHECK(command->risk == cmd::danger::deny_by_default);
	BOOST_CHECK(!edt::agent_may_use(*command));

	cmd::main_thread_queue queue{ actions.commands(), *tasks };
	edt::mcp_bridge_host bridge{ actions.commands(), queue, *tasks };

	std::optional<std::vector<mcp::tool_info>> tools;
	bridge.list_tools([&tools](std::vector<mcp::tool_info> listed) { tools = std::move(listed); });
	tasks->drain_main(std::chrono::hours(1));
	BOOST_REQUIRE(tools.has_value());
	for (const auto& tool : *tools) {
		BOOST_CHECK(tool.name.find("build") == std::string::npos);
	}

	std::optional<mcp::call_result> result;
	mcp::request_context context;
	context.session_id = "agent-1";
	bridge.call_tool(context, edt::agent_tool_name(*command), boost::json::object{}, nullptr,
		[&result](mcp::call_result r) { result = std::move(r); });
	tasks->drain_main(std::chrono::hours(1));
	BOOST_REQUIRE(result.has_value());
	BOOST_CHECK(result->is_error);
	BOOST_REQUIRE_EQUAL(result->content.size(), 1u);
	BOOST_CHECK(result->content[0].text.find("unknown tool") != std::string::npos);

	run_to_end();
	BOOST_CHECK(!builder->is_building());
	BOOST_CHECK(calls.empty());
	BOOST_CHECK_EQUAL(toolchain_lookups, 0);
}

BOOST_AUTO_TEST_CASE(Cancel_ShowsNothingAndLeavesTheProblemsAlone)
{
	make();
	problems.set("desc", { edt::problem{ edt::problem_severity::warning, "a.desc", 1, 0, {}, "kept", {} } });
	bool stop_seen = false;
	script = [this, &stop_seen](std::size_t, const std::function<void(std::string_view)>& on_line, std::stop_token stop) {
		on_line("-- Configuring");
		builder->cancel();
		stop_seen = stop.stop_requested();
		platform::process_result result;
		result.cancelled = true;
		return result;
	};
	BOOST_REQUIRE(build().ok);
	const auto generation = problems.generation();
	run_to_end();

	BOOST_CHECK(stop_seen);
	BOOST_CHECK_EQUAL(calls.size(), 1u);
	BOOST_CHECK(messages().empty());
	BOOST_CHECK_EQUAL(problems.generation(), generation);
	BOOST_CHECK_EQUAL(problems.size(), 1u);
	BOOST_CHECK(!builder->is_building());
	BOOST_CHECK(logged("build cancelled"));
}

BOOST_AUTO_TEST_CASE(SecondInvoke_WhileRunning_IsBusy)
{
	make();
	BOOST_REQUIRE(build().ok);
	const auto second = build();
	BOOST_CHECK(!second.ok);
	BOOST_CHECK_EQUAL(second.code, "busy");
	run_to_end();
	BOOST_CHECK_EQUAL(calls.size(), 2u);
	BOOST_CHECK(build().ok); // finished: a new build may start
}

BOOST_AUTO_TEST_CASE(Disabled_WithoutAProject)
{
	has_project = false;
	make();
	BOOST_CHECK(!actions.is_enabled(edt::game_builder::k_action_id));
	BOOST_CHECK_EQUAL(builder->disabled_reason(), "Open a project first");
	BOOST_CHECK_EQUAL(build().code, "disabled");
	BOOST_CHECK(calls.empty());
}

BOOST_AUTO_TEST_CASE(Disabled_WithoutPresets)
{
	fs::remove(root / "code" / "CMakePresets.json");
	make();
	BOOST_CHECK(!actions.is_enabled(edt::game_builder::k_action_id));
	BOOST_CHECK_EQUAL(builder->disabled_reason(), "The project has no code/CMakePresets.json");
}

BOOST_AUTO_TEST_CASE(Disabled_WithAStaticEngine)
{
	auto opts = options();
	opts.engine_shared = false;
	make(std::move(opts));
	BOOST_CHECK(!actions.is_enabled(edt::game_builder::k_action_id));
	BOOST_CHECK_EQUAL(builder->disabled_reason(), "Game modules need the shared engine build (SNK_ENGINE_SHARED)");
	const auto items = actions.items("Build");
	BOOST_REQUIRE_EQUAL(items.size(), 1u);
	BOOST_CHECK_EQUAL(items[0].label, "Build Game");
	BOOST_CHECK_EQUAL(items[0].shortcut_text, "Ctrl+B");
	BOOST_CHECK_EQUAL(items[0].disabled_tooltip, "Game modules need the shared engine build (SNK_ENGINE_SHARED)");
}

BOOST_AUTO_TEST_CASE(Enabled_WithProjectPresetsAndSharedEngine)
{
	make();
	BOOST_CHECK(actions.is_enabled(edt::game_builder::k_action_id));
	BOOST_CHECK(builder->disabled_reason().empty());
	const auto* desc = actions.find(edt::game_builder::k_action_id);
	BOOST_REQUIRE(desc != nullptr);
	BOOST_CHECK(desc->mutates);
	BOOST_CHECK(desc->expose_as_tool);
}

BOOST_AUTO_TEST_CASE(NinjaProgress_IsParsed)
{
	BOOST_CHECK((edt::parse_ninja_progress("[3/10] Building CXX object x.o") == std::optional<std::pair<int, int>>{ std::pair{ 3, 10 } }));
	BOOST_CHECK(!edt::parse_ninja_progress("[x/10] nope").has_value());
	BOOST_CHECK(!edt::parse_ninja_progress("-- Configuring done").has_value());
	BOOST_CHECK(!edt::parse_ninja_progress("[11/10] too far").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
