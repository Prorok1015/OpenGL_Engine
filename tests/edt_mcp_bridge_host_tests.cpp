#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "edt_agent_access.h"
#include "edt_agent_confirmations.h"
#include "edt_agent_policy.h"
#include "test_temp_dir.hpp"
#include "edt_mcp_bridge_host.h"
#include "mcp_test_support.hpp"
#include <snk/editor/edt_editor_events.h>
#include <snk/mcp/mcp_endpoint.h>
#include <snk/command/cmd_registry.h>
#include <filesystem>
#include <fstream>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// edt::mcp_bridge_host: the editor's side of the MCP server. Which commands an agent sees (decided by the agent policy,
// fail closed), the questions and the waiting behind a call, the end of an MCP session, and that every call hops to the main thread. The task system is manual here, so the test is the frame loop and the
// worker thread that stands for the server's I/O thread is an ordinary std::thread.

namespace {

namespace json = boost::json;

json::value json_of(std::string_view text)
{
	return json::parse(text);
}

std::string text_of(const json::value& value)
{
	const json::string& text = value.as_string();
	return std::string(text.data(), text.size());
}

constexpr const char* capture_result_schema = R"({
	"type": "object",
	"properties": {
		"viewport": { "type": "string" },
		"width": { "type": "integer" },
		"height": { "type": "integer" },
		"format": { "type": "string" },
		"png_base64": { "type": "string" }
	},
	"required": ["viewport", "width", "height", "format", "png_base64"],
	"additionalProperties": false
})";

constexpr const char* capture_args_schema = R"({"type":"object","properties":{"viewport":{"type":"string"},"width":{"type":"integer"},"height":{"type":"integer"},"max_side":{"type":"integer"},"max_pixels":{"type":"integer"},"max_source_pixels":{"type":"integer"}},"additionalProperties":false})";

struct bridge_fixture
{
	test_util::temp_dir dir{ "edt_mcp_bridge" };
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	std::chrono::steady_clock::time_point current{};
	edt::agent_access access{ commands, dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json", [this] { return current; } };
	edt::editor_events events;
	std::vector<std::string> closed_sessions;
	edt::connection session_listener;
	edt::mcp_bridge_host bridge{ commands, tasks, access, [this](const std::string& session) { events.agent_session_closed.emit(session); } };
	std::vector<cmd::registration> registrations;

	bridge_fixture()
	{
		access.set_questions_routed(true);
		// Wired the way editor_system wires it: one signal, the confirmations listen to it.
		session_listener = events.agent_session_closed.subscribe([this](const std::string& session) {
			closed_sessions.push_back(session);
			access.confirmations().drop_session(session);
		});
	}

	~bridge_fixture()
	{
		session_listener.disconnect();
	}

	std::size_t drain()
	{
		return tasks.drain_main(std::chrono::hours(1));
	}

	void write_project_policy(std::string_view text)
	{
		const std::filesystem::path file = dir.path() / ".snk" / "agent_policy.json";
		std::filesystem::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out << text;
		out.close();
		// The file watcher compares the time and the size: make a rewrite visible even within one clock tick.
		std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++rewrites));
		access.policy().refresh();
	}

	int rewrites = 0;

	cmd::command_desc make(std::string id)
	{
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.category = cmd::agent_category::read;
		desc.summary = "A command";
		desc.invoke = [](const cmd::call&) { return cmd::result::success(); };
		return desc;
	}

	void add(cmd::command_desc desc)
	{
		cmd::registration registration = commands.add(std::move(desc));
		BOOST_REQUIRE(registration.active());
		registrations.push_back(std::move(registration));
	}

	std::vector<mcp::tool_info> list_tools()
	{
		std::optional<std::vector<mcp::tool_info>> tools;
		bridge.list_tools([&tools](std::vector<mcp::tool_info> listed) { tools = std::move(listed); });
		BOOST_REQUIRE_MESSAGE(!tools.has_value(), "list_tools answered before the main-thread drain");
		drain();
		BOOST_REQUIRE(tools.has_value());
		return std::move(*tools);
	}

	mcp::call_result call_tool(const std::string& name, const std::string& args = "{}", const std::string& session = "session-1")
	{
		std::optional<mcp::call_result> result;
		mcp::request_context context;
		context.session_id = session;
		bridge.call_tool(context, name, json_of(args), nullptr, [&result](mcp::call_result r) { result = std::move(r); });
		drain();
		BOOST_REQUIRE(result.has_value());
		return std::move(*result);
	}

	mcp::resource_result read_resource(const std::string& uri)
	{
		std::optional<mcp::resource_result> result;
		bridge.read_resource({}, uri, [&result](mcp::resource_result r) { result = std::move(r); });
		drain();
		BOOST_REQUIRE(result.has_value());
		return std::move(*result);
	}
};

std::vector<std::string> names_of(const std::vector<mcp::tool_info>& tools)
{
	std::vector<std::string> names;
	for (const mcp::tool_info& tool : tools) {
		names.push_back(tool.name);
	}
	return names;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentPolicyTests, bridge_fixture)

BOOST_AUTO_TEST_CASE(ReadCommands_AreListedByDefault)
{
	for (const char* id : { "log.read", "job.list", "job.get", "dialog.list", "viewport.capture", "scene.query", "problems.list", "vcs.status" }) {
		cmd::command_desc desc = make(id);
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST(edt::agent_may_use(desc, access.policy()));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheOwnersDefaultTable_DecidesWhatElseIsListed)
{
	using cmd::agent_category;
	const std::pair<agent_category, bool> table[] = {
		{ agent_category::read, true }, { agent_category::scene_edit, true }, { agent_category::code_edit, true },
		{ agent_category::code_files, true }, { agent_category::cmake_presets, true }, { agent_category::build, false },
		{ agent_category::live, false },
	};
	for (const auto& [category, listed] : table) {
		cmd::command_desc desc = make("x.y");
		desc.category = category;
		desc.mutates = category != agent_category::read;
		BOOST_TEST_CONTEXT(cmd::to_string(category)) {
			BOOST_TEST(edt::agent_may_use(desc, access.policy()) == listed);
		}
	}
}

BOOST_AUTO_TEST_CASE(ACommandWithoutACategoryOrForNobody_IsHiddenWhateverThePolicySays)
{
	write_project_policy(R"({"version":1,"categories":{"read":"allow","scene_edit":"allow","build":"allow"},"commands":{"a.unset":"allow","a.nobody":"allow"}})");
	cmd::command_desc unset = make("a.unset");
	unset.category = cmd::agent_category::unset;
	cmd::command_desc nobody = make("a.nobody");
	nobody.category = cmd::agent_category::not_for_agents;
	BOOST_TEST(!edt::agent_may_use(unset, access.policy()));
	BOOST_TEST(!edt::agent_may_use(nobody, access.policy()));
}

BOOST_AUTO_TEST_CASE(ACommandNotOfferedAsATool_IsHidden)
{
	cmd::command_desc desc = make("log.read");
	desc.expose_as_tool = false;
	BOOST_TEST(!edt::agent_may_use(desc, access.policy()));
}

BOOST_AUTO_TEST_CASE(BuildCommands_AreHiddenByDefault_AndListedOnlyWhenThePersonAllowedThem)
{
	cmd::command_desc build = make("build.cleanup");
	build.category = cmd::agent_category::build;
	build.mutates = true;
	BOOST_TEST(!edt::agent_may_use(build, access.policy()));

	write_project_policy(R"({"version":1,"categories":{"build":"allow"}})");
	BOOST_TEST(edt::agent_may_use(build, access.policy()));

	// A command that runs the project's CMake code stays at deny until its very own entry says otherwise.
	cmd::command_desc risky = make("build.game_module");
	risky.category = cmd::agent_category::build;
	risky.mutates = true;
	risky.risk = cmd::danger::deny_by_default;
	BOOST_TEST(!edt::agent_may_use(risky, access.policy()));
	write_project_policy(R"({"version":1,"categories":{"build":"allow"},"commands":{"build.game_module":"ask"}})");
	BOOST_TEST(edt::agent_may_use(risky, access.policy()));
}

BOOST_AUTO_TEST_CASE(ABrokenPolicyFile_HidesEverythingEvenReads)
{
	write_project_policy("{ this is not json");
	BOOST_TEST(!edt::agent_may_use(make("log.read"), access.policy()));
	cmd::command_desc scene = make("scene.node.rename");
	scene.category = cmd::agent_category::scene_edit;
	BOOST_TEST(!edt::agent_may_use(scene, access.policy()));
}

BOOST_AUTO_TEST_CASE(TheLiveSwitch_IsTheConsentForTheLiveTools)
{
	bool live_on = false;
	access.policy().set_live_mode([&live_on] { return live_on; });
	cmd::command_desc status = make("live.status");
	status.category = cmd::agent_category::live;
	int runs = 0;
	status.invoke = [&runs](const cmd::call&) {
		++runs;
		return cmd::result::success(json::object{});
	};
	add(std::move(status));

	BOOST_TEST(list_tools().empty());
	const mcp::call_result refused = call_tool("live_status");
	BOOST_TEST(refused.is_error);
	BOOST_TEST(runs == 0);

	live_on = true;
	BOOST_TEST((names_of(list_tools()) == std::vector<std::string>{ "live_status" }));
	BOOST_TEST(!call_tool("live_status").is_error);
	BOOST_TEST(runs == 1);

	live_on = false;
	BOOST_TEST(list_tools().empty());
	BOOST_TEST(call_tool("live_status").is_error);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_CASE(ToolName_IsTheIdWithUnderscoresAndTheVersion)
{
	cmd::command_desc desc;
	desc.id = "viewport.capture";
	BOOST_TEST(edt::agent_tool_name(desc) == "viewport_capture");
	desc.version = 2;
	BOOST_TEST(edt::agent_tool_name(desc) == "viewport_capture_v2");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtMcpBridgeToolsTests, bridge_fixture)

BOOST_AUTO_TEST_CASE(ListTools_ExposesWhatThePolicyAllows)
{
	cmd::command_desc read = make("log.read");
	read.args_schema = json_of(R"({"type":"object","properties":{"limit":{"type":"integer"}},"additionalProperties":false})");
	read.result_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
	add(std::move(read));
	// No category: nobody classified it.
	cmd::command_desc unclassified = make("job.cancel");
	unclassified.mutates = true;
	unclassified.category = cmd::agent_category::unset;
	add(std::move(unclassified));
	cmd::command_desc for_people = make("file.save");
	for_people.category = cmd::agent_category::not_for_agents;
	add(std::move(for_people));
	// A category the owner's table denies.
	cmd::command_desc build = make("app.advance");
	build.category = cmd::agent_category::build;
	add(std::move(build));
	cmd::command_desc hidden = make("dialog.list");
	hidden.expose_as_tool = false;
	add(std::move(hidden));
	add(make("job.list"));

	const std::vector<mcp::tool_info> tools = list_tools();
	BOOST_TEST((names_of(tools) == std::vector<std::string>{ "log_read", "job_list" }));
	BOOST_TEST(tools[0].description == "A command");
	BOOST_TEST(tools[0].read_only);
	BOOST_TEST(!tools[0].destructive);
	BOOST_TEST(!tools[0].idempotent);
	BOOST_TEST(!tools[0].open_world);
	BOOST_TEST(json::serialize(tools[0].input_schema) == R"({"type":"object","properties":{"limit":{"type":"integer"}},"additionalProperties":false})");
	BOOST_TEST(tools[0].output_schema.is_object());
}

BOOST_AUTO_TEST_CASE(ListTools_AnnotationsSayWhatAToolDoes)
{
	cmd::command_desc edit = make("scene.node.rename");
	edit.category = cmd::agent_category::scene_edit;
	edit.mutates = true;
	add(std::move(edit));
	cmd::command_desc risky = make("code.delete");
	risky.category = cmd::agent_category::code_files;
	risky.mutates = true;
	risky.risk = cmd::danger::confirm;
	add(std::move(risky));
	// Marked destructive by itself (it removes a file), though nothing about its risk says so.
	cmd::command_desc removes = make("code.rename");
	removes.category = cmd::agent_category::code_files;
	removes.mutates = true;
	removes.destructive = true;
	add(std::move(removes));
	// A read command is never destructive, whatever the flag says.
	cmd::command_desc odd = make("log.read");
	odd.destructive = true;
	add(std::move(odd));

	const std::vector<mcp::tool_info> tools = list_tools();
	BOOST_REQUIRE_EQUAL(tools.size(), 4u);
	for (const mcp::tool_info& tool : tools) {
		BOOST_TEST_CONTEXT(tool.name) {
			BOOST_TEST(tool.read_only == (tool.name == "log_read"));
			BOOST_TEST(!tool.open_world);
			BOOST_TEST(tool.destructive == (tool.name == "code_delete" || tool.name == "code_rename"));
		}
	}
}

BOOST_AUTO_TEST_CASE(ListTools_FollowsTheRegistry)
{
	BOOST_TEST(list_tools().empty());
	add(make("log.read"));
	BOOST_TEST(list_tools().size() == 1u);
	registrations.clear();
	BOOST_TEST(list_tools().empty());
}

BOOST_AUTO_TEST_CASE(ListTools_ACaptureToolDeclaresNoOutputSchema)
{
	cmd::command_desc capture = make("viewport.capture");
	capture.result_schema = json_of(capture_result_schema);
	add(std::move(capture));
	const std::vector<mcp::tool_info> tools = list_tools();
	BOOST_REQUIRE_EQUAL(tools.size(), 1u);
	BOOST_TEST(tools[0].output_schema.is_null());
}

BOOST_AUTO_TEST_CASE(CallTool_RunsOnTheMainThreadInTheDrainAsTheAgent)
{
	std::thread::id ran_on;
	cmd::origin seen;
	int runs = 0;
	cmd::command_desc desc = make("log.read");
	desc.args_schema = json_of(R"({"type":"object","properties":{"limit":{"type":"integer"}},"additionalProperties":false})");
	desc.result_schema = json_of(R"({"type":"object","properties":{"limit":{"type":"integer"}},"required":["limit"]})");
	desc.invoke = [&](const cmd::call& request) {
		++runs;
		ran_on = std::this_thread::get_id();
		seen = request.from;
		return cmd::result::success(json::object{ { "limit", request.args.as_object().at("limit") } });
	};
	add(std::move(desc));

	// From another thread, the way the server's I/O thread calls it.
	std::optional<mcp::call_result> result;
	std::thread::id caller;
	std::thread worker([&] {
		caller = std::this_thread::get_id();
		mcp::request_context context;
		context.session_id = "abc";
		bridge.call_tool(context, "log_read", json_of(R"({"limit":3})"), nullptr, [&result](mcp::call_result r) { result = std::move(r); });
	});
	worker.join();
	BOOST_TEST(runs == 0);
	BOOST_TEST(!result.has_value());
	BOOST_TEST(tasks.pending_main(ds::main_class::general) == 1u);

	BOOST_TEST(drain() == 1u);
	BOOST_REQUIRE(result.has_value());
	BOOST_TEST(runs == 1);
	BOOST_TEST((ran_on == std::this_thread::get_id()));
	BOOST_TEST((ran_on != caller));
	BOOST_TEST((seen == cmd::origin::agent("abc")));
	BOOST_TEST(!result->is_error);
	BOOST_TEST(json::serialize(result->structured) == R"({"limit":3})");
}

BOOST_AUTO_TEST_CASE(CallTool_AToolThatIsNotListedDoesNotExist)
{
	int runs = 0;
	const std::pair<const char*, cmd::agent_category> hidden_commands[] = {
		{ "job.cancel", cmd::agent_category::unset }, { "file.save", cmd::agent_category::not_for_agents },
		{ "app.advance", cmd::agent_category::build },
	};
	for (const auto& [id, category] : hidden_commands) {
		cmd::command_desc desc = make(id);
		desc.category = category;
		desc.mutates = std::string(id) == "job.cancel";
		desc.invoke = [&runs](const cmd::call&) {
			++runs;
			return cmd::result::success();
		};
		add(std::move(desc));
	}
	for (const char* name : { "job_cancel", "file_save", "app_advance", "job.cancel", "nothing" }) {
		const mcp::call_result result = call_tool(name);
		BOOST_TEST_CONTEXT(name) {
			BOOST_TEST(result.is_error);
			BOOST_REQUIRE_EQUAL(result.content.size(), 1u);
			BOOST_TEST(result.content[0].text.find("unknown tool") != std::string::npos);
		}
	}
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(CallTool_AFailureIsAnErrorResultWithTheCode)
{
	cmd::command_desc desc = make("log.read");
	desc.args_schema = json_of(R"({"type":"object","properties":{"limit":{"type":"integer"}},"additionalProperties":false})");
	add(std::move(desc));
	cmd::command_desc failing = make("job.get");
	failing.invoke = [](const cmd::call&) { return cmd::result::failure(cmd::codes::not_found, "no job 7"); };
	add(std::move(failing));
	cmd::command_desc throwing = make("job.list");
	throwing.invoke = [](const cmd::call&) -> cmd::result { throw std::runtime_error("boom"); };
	add(std::move(throwing));

	const mcp::call_result invalid = call_tool("log_read", R"({"limit":"many"})");
	BOOST_TEST(invalid.is_error);
	BOOST_REQUIRE_EQUAL(invalid.content.size(), 1u);
	BOOST_TEST(invalid.content[0].text.rfind("invalid_args", 0) == 0u);

	const mcp::call_result missing = call_tool("job_get");
	BOOST_TEST(missing.is_error);
	BOOST_TEST(missing.content.at(0).text == "not_found: no job 7");

	const mcp::call_result thrown = call_tool("job_list");
	BOOST_TEST(thrown.is_error);
	BOOST_TEST(thrown.content.at(0).text.find("boom") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CallTool_ACapturedPngBecomesAnImageBlock)
{
	cmd::command_desc capture = make("viewport.capture");
	capture.args_schema = json_of(capture_args_schema);
	capture.result_schema = json_of(capture_result_schema);
	capture.invoke = [](const cmd::call&) {
		return cmd::result::success(json_of(R"({"viewport":"scene","width":2,"height":1,"format":"png","png_base64":"QUJD"})"));
	};
	add(std::move(capture));

	const mcp::call_result result = call_tool("viewport_capture");
	BOOST_TEST(!result.is_error);
	BOOST_REQUIRE_EQUAL(result.content.size(), 2u);
	BOOST_TEST((result.content[0].type == mcp::content_block::kind::image));
	BOOST_TEST(result.content[0].data == "QUJD");
	BOOST_TEST(result.content[0].mime_type == "image/png");
	BOOST_TEST((result.content[1].type == mcp::content_block::kind::text));
	BOOST_REQUIRE(result.structured.is_object());
	BOOST_TEST(!result.structured.as_object().contains("png_base64"));
	BOOST_TEST(result.structured.at("width").as_int64() == 2);
	BOOST_TEST(text_of(result.structured.at("viewport")) == "scene");
}

BOOST_AUTO_TEST_CASE(CallTool_ACaptureSizeIsLimitedForAnAgent)
{
	std::vector<std::string> seen;
	cmd::command_desc capture = make("viewport.capture");
	capture.args_schema = json_of(capture_args_schema);
	capture.invoke = [&seen](const cmd::call& request) {
		seen.push_back(json::serialize(request.args));
		return cmd::result::success();
	};
	add(std::move(capture));

	for (const char* args : { R"({"width":4096})", R"({"height":2049})", R"({"width":8192,"height":8192})", R"({"viewport":"scene","width":2049})" }) {
		const mcp::call_result refused = call_tool("viewport_capture", args);
		BOOST_TEST_CONTEXT(args) {
			BOOST_TEST(refused.is_error);
			BOOST_REQUIRE_EQUAL(refused.content.size(), 1u);
			BOOST_TEST(refused.content[0].text.find("limited to 2048") != std::string::npos);
		}
	}
	BOOST_TEST(seen.empty());

	BOOST_TEST(!call_tool("viewport_capture", R"({"width":2048,"height":2048})").is_error);
	BOOST_TEST(!call_tool("viewport_capture", R"({"height":600})").is_error);
	// No size named: a default width, not the native size of a possibly huge view.
	BOOST_TEST(!call_tool("viewport_capture", "{}").is_error);
	BOOST_REQUIRE_EQUAL(seen.size(), 3u);
	// The ceilings travel with the request: the command derives the side the agent did not name and checks it against them.
	BOOST_TEST(seen[0] == R"({"width":2048,"height":2048,"max_side":2048,"max_pixels":4194304,"max_source_pixels":8388608})");
	BOOST_TEST(seen[1] == R"({"height":600,"max_side":2048,"max_pixels":4194304,"max_source_pixels":8388608})");
	BOOST_TEST(seen[2] == R"({"width":1280,"max_side":2048,"max_pixels":4194304,"max_source_pixels":8388608})");
}

BOOST_AUTO_TEST_CASE(CallTool_AnAgentCanLowerTheCaptureCeilingsNeverRaiseThem)
{
	std::vector<std::string> seen;
	cmd::command_desc capture = make("viewport.capture");
	capture.args_schema = json_of(capture_args_schema);
	capture.invoke = [&seen](const cmd::call& request) {
		seen.push_back(json::serialize(request.args));
		return cmd::result::success();
	};
	add(std::move(capture));

	BOOST_TEST(!call_tool("viewport_capture", R"({"width":100,"max_side":512,"max_pixels":1000})").is_error);
	BOOST_TEST(!call_tool("viewport_capture", R"({"width":100,"max_side":8192,"max_pixels":33554432,"max_source_pixels":33554432})").is_error);
	BOOST_REQUIRE_EQUAL(seen.size(), 2u);
	BOOST_TEST(seen[0] == R"({"width":100,"max_side":512,"max_pixels":1000,"max_source_pixels":8388608})");
	BOOST_TEST(seen[1] == R"({"width":100,"max_side":2048,"max_pixels":4194304,"max_source_pixels":8388608})");
}

BOOST_AUTO_TEST_CASE(CallTool_ATooLargeResultIsAnErrorNotAMegabyteWall)
{
	cmd::command_desc big = make("log.read");
	big.result_schema = json_of(R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})");
	big.invoke = [](const cmd::call&) {
		return cmd::result::success(json::object{ { "text", std::string(17u * 1024u * 1024u, 'x') } });
	};
	add(std::move(big));
	cmd::command_desc capture = make("viewport.capture");
	capture.args_schema = json_of(capture_args_schema);
	capture.result_schema = json_of(capture_result_schema);
	capture.invoke = [](const cmd::call&) {
		return cmd::result::success(json::object{ { "viewport", "scene" }, { "width", 1 }, { "height", 1 }, { "format", "png" },
			{ "png_base64", std::string(17u * 1024u * 1024u, 'A') } });
	};
	add(std::move(capture));

	const mcp::call_result text = call_tool("log_read");
	BOOST_TEST(text.is_error);
	BOOST_REQUIRE_EQUAL(text.content.size(), 1u);
	BOOST_TEST(text.content[0].text.find("too large") != std::string::npos);
	BOOST_TEST(text.structured.is_null());

	const mcp::call_result image = call_tool("viewport_capture", R"({"width":10})");
	BOOST_TEST(image.is_error);
	BOOST_REQUIRE_EQUAL(image.content.size(), 1u);
	BOOST_TEST((image.content[0].type == mcp::content_block::kind::text));
	BOOST_TEST(image.content[0].text.find("too large") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HostGoneBeforeTheDrain_AnswersWithAnError)
{
	add(make("log.read"));
	auto doomed = std::make_unique<edt::mcp_bridge_host>(commands, tasks, access);
	std::optional<mcp::call_result> result;
	std::optional<std::vector<mcp::tool_info>> tools;
	doomed->call_tool({}, "log_read", json_of("{}"), nullptr, [&result](mcp::call_result r) { result = std::move(r); });
	doomed->list_tools([&tools](std::vector<mcp::tool_info> listed) { tools = std::move(listed); });
	doomed.reset();

	drain();
	BOOST_REQUIRE(result.has_value());
	BOOST_TEST(result->is_error);
	BOOST_REQUIRE(tools.has_value());
	BOOST_TEST(tools->empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtMcpBridgeResourcesTests, bridge_fixture)

BOOST_AUTO_TEST_CASE(ListResources_OnlyThoseWhoseCommandExists)
{
	std::optional<std::vector<mcp::resource_info>> listed;
	bridge.list_resources([&listed](std::vector<mcp::resource_info> r) { listed = std::move(r); });
	drain();
	BOOST_REQUIRE(listed.has_value());
	BOOST_TEST(listed->empty());

	add(make("log.read"));
	cmd::command_desc capture = make("viewport.capture");
	capture.result_schema = json_of(capture_result_schema);
	add(std::move(capture));
	listed.reset();
	bridge.list_resources([&listed](std::vector<mcp::resource_info> r) { listed = std::move(r); });
	drain();
	BOOST_REQUIRE(listed.has_value());
	std::vector<std::string> uris;
	for (const mcp::resource_info& r : *listed) {
		uris.push_back(r.uri);
	}
	BOOST_TEST((uris == std::vector<std::string>{ "snk://log", "snk://viewport/scene.png", "snk://viewport/game.png" }));
}

BOOST_AUTO_TEST_CASE(ReadResource_RunsTheCommandAndReturnsTextOrABlob)
{
	std::vector<std::string> viewports;
	cmd::command_desc log = make("log.read");
	log.result_schema = json_of(R"({"type":"object","properties":{"entries":{"type":"array"}},"required":["entries"]})");
	log.invoke = [](const cmd::call&) { return cmd::result::success(json_of(R"({"entries":[]})")); };
	add(std::move(log));
	cmd::command_desc capture = make("viewport.capture");
	capture.args_schema = json_of(capture_args_schema);
	capture.result_schema = json_of(capture_result_schema);
	capture.invoke = [&viewports](const cmd::call& request) {
		viewports.push_back(text_of(request.args.as_object().at("viewport")));
		return cmd::result::success(json_of(R"({"viewport":"game","width":1,"height":1,"format":"png","png_base64":"QUJD"})"));
	};
	add(std::move(capture));

	const mcp::resource_result text = read_resource("snk://log");
	BOOST_REQUIRE(text.found);
	BOOST_REQUIRE_EQUAL(text.contents.size(), 1u);
	BOOST_TEST(text.contents[0].uri == "snk://log");
	BOOST_TEST(text.contents[0].mime_type == "application/json");
	BOOST_TEST(!text.contents[0].is_blob);
	BOOST_TEST(text.contents[0].text == R"({"entries":[]})");

	const mcp::resource_result image = read_resource("snk://viewport/game.png");
	BOOST_REQUIRE(image.found);
	BOOST_REQUIRE_EQUAL(image.contents.size(), 1u);
	BOOST_TEST(image.contents[0].is_blob);
	BOOST_TEST(image.contents[0].blob == "QUJD");
	BOOST_TEST(image.contents[0].mime_type == "image/png");
	BOOST_TEST((viewports == std::vector<std::string>{ "game" }));
}

BOOST_AUTO_TEST_CASE(ReadResource_UnknownOrUnavailableOrFailing)
{
	BOOST_TEST(!read_resource("snk://nope").found);
	// Known resource, command not registered.
	BOOST_TEST(!read_resource("snk://log").found);

	cmd::command_desc capture = make("viewport.capture");
	capture.args_schema = json_of(capture_args_schema);
	capture.invoke = [](const cmd::call&) { return cmd::result::failure(cmd::codes::not_found, "unknown viewport 'game'"); };
	add(std::move(capture));
	const mcp::resource_result failed = read_resource("snk://viewport/game.png");
	BOOST_TEST(!failed.found);
	BOOST_TEST(failed.error == "not_found: unknown viewport 'game'");
}

BOOST_AUTO_TEST_CASE(ReadResource_AMutatingCommandIsNeverRead)
{
	int runs = 0;
	cmd::command_desc log = make("log.read");
	log.mutates = true;
	log.invoke = [&runs](const cmd::call&) {
		++runs;
		return cmd::result::success();
	};
	add(std::move(log));
	BOOST_TEST(!read_resource("snk://log").found);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// A command of the code_files category: the owner's table says `ask`.
struct question_fixture : bridge_fixture
{
	int attempts = 0;
	int runs = 0;
	std::optional<mcp::call_result> answered;

	question_fixture()
	{
		access.set_person_present(true);
	}

	void add_code_command(std::function<cmd::result()> body = {})
	{
		cmd::command_desc desc = make("code.create");
		desc.category = cmd::agent_category::code_files;
		desc.mutates = true;
		desc.args_schema = json_of(R"({"type":"object"})");
		desc.invoke = [this, body = std::move(body)](const cmd::call&) {
			++attempts;
			if (body) {
				cmd::result outcome = body();
				if (outcome.ok) ++runs;
				return outcome;
			}
			++runs;
			return cmd::result::success();
		};
		add(std::move(desc));
	}

	void start_call(const std::string& name, const std::string& session = "session-1")
	{
		answered.reset();
		mcp::request_context context;
		context.session_id = session;
		bridge.call_tool(context, name, json_of(R"({"path":"a.cpp"})"), nullptr, [this](mcp::call_result r) { answered = std::move(r); });
		drain();
	}

	std::uint64_t the_question()
	{
		const auto waiting = access.confirmations().pending();
		BOOST_REQUIRE_EQUAL(waiting.size(), 1u);
		return waiting.front().id;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMcpBridgeQuestionTests, question_fixture)

BOOST_AUTO_TEST_CASE(AnAskedCall_WaitsForAPerson_ThenRunsOnce)
{
	add_code_command();
	start_call("code_create");

	BOOST_TEST(!answered.has_value());
	BOOST_TEST(runs == 0);
	const auto waiting = access.confirmations().pending();
	BOOST_REQUIRE_EQUAL(waiting.size(), 1u);
	BOOST_TEST(waiting[0].command == "code.create");
	BOOST_TEST(waiting[0].session == "session-1");

	BOOST_TEST(access.confirmations().answer(waiting[0].id, edt::confirmation_answer::allow_once));
	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(!answered->is_error);
	BOOST_TEST(runs == 1);
	BOOST_TEST(access.confirmations().pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(ADeclinedCall_NeverRuns)
{
	add_code_command();
	start_call("code_create");
	BOOST_TEST(access.confirmations().answer(the_question(), edt::confirmation_answer::deny));

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	BOOST_TEST(answered->content.at(0).text.rfind("denied", 0) == 0u);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(NobodyToAsk_AnAskIsADenyAtOnce)
{
	add_code_command();
	access.set_person_present(false);
	start_call("code_create");

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	BOOST_TEST(runs == 0);
	BOOST_TEST(access.confirmations().pending().empty());
}

BOOST_AUTO_TEST_CASE(NoAnswerInTime_IsADeny)
{
	add_code_command();
	start_call("code_create");
	current += edt::agent_confirmations::timeout + std::chrono::seconds(1);
	access.confirmations().tick();

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(InvalidArguments_AreRefusedWithoutAQuestion)
{
	cmd::command_desc desc = make("code.create");
	desc.category = cmd::agent_category::code_files;
	desc.mutates = true;
	desc.args_schema = json_of(R"({"type":"object","properties":{"path":{"type":"string"}},"additionalProperties":false})");
	add(std::move(desc));

	mcp::request_context context;
	context.session_id = "session-1";
	bridge.call_tool(context, "code_create", json_of(R"({"path":7})"), nullptr, [this](mcp::call_result r) { answered = std::move(r); });
	drain();

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	BOOST_TEST(answered->content.at(0).text.rfind("invalid_args", 0) == 0u);
	BOOST_TEST(access.confirmations().pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(TheEndOfTheSession_DeclinesItsWaitingCallAndTellsTheEditorOnce)
{
	add_code_command();
	start_call("code_create", "gone");
	BOOST_REQUIRE(!answered.has_value());

	bridge.session_closed("gone");
	BOOST_TEST(closed_sessions.empty());
	drain();

	BOOST_TEST((closed_sessions == std::vector<std::string>{ "gone" }));
	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	BOOST_TEST(runs == 0);
	BOOST_TEST(access.confirmations().pending().empty());
}

BOOST_AUTO_TEST_CASE(TheEndOfTheSession_LeavesAnotherSessionsQuestionAlone)
{
	add_code_command();
	start_call("code_create", "kept");
	std::optional<mcp::call_result> kept = std::move(answered);
	BOOST_REQUIRE(!kept.has_value());

	bridge.session_closed("other");
	drain();

	BOOST_TEST(access.confirmations().pending().size() == 1u);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(ABusyCommandAfterAnApproval_IsTriedAgainWithoutAskingTheSecondTime)
{
	// The person has just clicked "Allow": for a frame an ImGui item is active and the command says `busy`.
	add_code_command([this] {
		return attempts == 1 ? cmd::result::failure(cmd::codes::busy, "a person is in the middle of an edit") : cmd::result::success();
	});
	start_call("code_create");
	BOOST_TEST(access.confirmations().answer(the_question(), edt::confirmation_answer::allow_once));

	BOOST_TEST(!answered.has_value());
	BOOST_TEST(attempts == 1);
	BOOST_TEST(runs == 0);
	BOOST_TEST(access.confirmations().pending().empty());

	access.confirmations().tick();

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(!answered->is_error);
	BOOST_TEST(attempts == 2);
	BOOST_TEST(runs == 1);
	BOOST_TEST(access.confirmations().pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(ABusyCommandAfterAnApproval_GivesUpAfterFiveSeconds)
{
	add_code_command([] { return cmd::result::failure(cmd::codes::busy, "a person is in the middle of an edit"); });
	start_call("code_create");
	BOOST_TEST(access.confirmations().answer(the_question(), edt::confirmation_answer::allow_once));

	current += std::chrono::seconds(2);
	access.confirmations().tick();
	BOOST_TEST(!answered.has_value());

	current += edt::agent_confirmations::busy_wait;
	access.confirmations().tick();

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	BOOST_TEST(answered->content.at(0).text.rfind("busy", 0) == 0u);
	BOOST_TEST(runs == 0);
	BOOST_TEST(access.confirmations().pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(ABusyCommandThatNeedsNoQuestion_IsTriedAgainEveryFrame)
{
	cmd::command_desc desc = make("scene.node.rename");
	desc.category = cmd::agent_category::scene_edit;
	desc.mutates = true;
	desc.args_schema = json_of(R"({"type":"object"})");
	desc.invoke = [this](const cmd::call&) {
		++attempts;
		if (attempts < 3) return cmd::result::failure(cmd::codes::busy, "a person is in the middle of an edit");
		++runs;
		return cmd::result::success();
	};
	add(std::move(desc));
	start_call("scene_node_rename");
	BOOST_TEST(!answered.has_value());
	BOOST_TEST(attempts == 1);

	access.confirmations().tick();
	BOOST_TEST(!answered.has_value());
	access.confirmations().tick();

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(!answered->is_error);
	BOOST_TEST(attempts == 3);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_CASE(AWaitingBusyCall_EndsWithItsSession)
{
	cmd::command_desc desc = make("scene.node.rename");
	desc.category = cmd::agent_category::scene_edit;
	desc.mutates = true;
	desc.args_schema = json_of(R"({"type":"object"})");
	desc.invoke = [this](const cmd::call&) {
		++attempts;
		return cmd::result::failure(cmd::codes::busy, "a person is in the middle of an edit");
	};
	add(std::move(desc));
	start_call("scene_node_rename", "gone");
	BOOST_TEST(!answered.has_value());

	bridge.session_closed("gone");
	drain();

	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(answered->is_error);
	access.confirmations().tick();
	BOOST_TEST(attempts == 1);
}

BOOST_AUTO_TEST_CASE(ASessionGrant_AllowsTheNextCallOfThatCategoryWithoutAQuestion)
{
	add_code_command();
	start_call("code_create");
	BOOST_TEST(access.confirmations().answer(the_question(), edt::confirmation_answer::allow_session));
	BOOST_REQUIRE(answered.has_value());

	start_call("code_create");
	BOOST_REQUIRE(answered.has_value());
	BOOST_TEST(!answered->is_error);
	BOOST_TEST(runs == 2);
	BOOST_TEST(access.confirmations().pending().empty());

	start_call("code_create", "another-session");
	BOOST_TEST(!answered.has_value());
	BOOST_TEST(access.confirmations().pending().size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtMcpBridgeSessionEndTests, bridge_fixture)

namespace {

mcp::http_request endpoint_request(std::string method, std::string body)
{
	mcp::http_request request;
	request.method = std::move(method);
	request.target = "/mcp";
	request.body = std::move(body);
	request.headers.push_back({ "host", "127.0.0.1:4711" });
	request.headers.push_back({ "authorization", std::string("Bearer ") + mcp_test::test_token });
	request.headers.push_back({ "content-type", "application/json" });
	return request;
}

std::string open_session(mcp::endpoint& endpoint)
{
	std::string session;
	endpoint.handle(endpoint_request("POST", R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})"),
		[&session](mcp::http_response response) {
			for (const mcp::http_header& header : response.headers) {
				if (header.name == "Mcp-Session-Id") session = header.value;
			}
		});
	return session;
}

}

BOOST_AUTO_TEST_CASE(EveryWayASessionEnds_ReachesTheEditorOnce)
{
	std::chrono::steady_clock::time_point clock{};
	mcp::endpoint_config config;
	config.token = mcp_test::test_token;
	config.info.name = "test";
	config.session_idle_timeout = std::chrono::minutes(30);
	config.clock = [&clock] { return clock; };
	mcp::endpoint endpoint(bridge, config);
	endpoint.set_port(4711);

	const std::string disconnected = open_session(endpoint);
	const std::string idle = open_session(endpoint);
	const std::string kept = open_session(endpoint);
	BOOST_REQUIRE(!disconnected.empty() && !idle.empty() && !kept.empty());

	// An explicit close (DELETE).
	mcp::http_request close = endpoint_request("DELETE", {});
	close.headers.push_back({ "mcp-session-id", disconnected });
	endpoint.handle(close, [](mcp::http_response) {});
	drain();
	BOOST_TEST((closed_sessions == std::vector<std::string>{ disconnected }));

	// The idle timeout (30 minutes without a request); `kept` is used in between.
	clock += std::chrono::minutes(20);
	mcp::http_request ping = endpoint_request("POST", R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
	ping.headers.push_back({ "mcp-session-id", kept });
	endpoint.handle(ping, [](mcp::http_response) {});
	clock += std::chrono::minutes(15);
	endpoint.expire_idle_sessions();
	drain();
	BOOST_TEST((closed_sessions == std::vector<std::string>{ disconnected, idle }));

	// The server stopping with the editor tells nobody: the listeners are going too.
	bridge.stop_reporting();
	endpoint.close_all();
	drain();
	BOOST_TEST(closed_sessions.size() == 2u);
}

BOOST_AUTO_TEST_CASE(ASessionEndingAfterTheBridgeIsGone_ReachesNobody)
{
	auto doomed = std::make_unique<edt::mcp_bridge_host>(commands, tasks, access,
		[this](const std::string& session) { closed_sessions.push_back(session); });
	doomed->session_closed("late");
	doomed.reset();
	BOOST_CHECK_NO_THROW(drain());
	BOOST_TEST(closed_sessions.empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtMcpBridgeTeardownTests)

// The editor goes away while a call waits for a person (run under ASan): the answer still reaches the client's closure,
// and that closure holds neither the bridge nor the access.
BOOST_AUTO_TEST_CASE(TheEditorClosingWithAWaitingCall_DeclinesItAndTouchesNothingFreed)
{
	test_util::temp_dir dir{ "edt_mcp_bridge_teardown" };
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "code.create";
	desc.category = cmd::agent_category::code_files;
	desc.mutates = true;
	desc.args_schema = boost::json::value{ { "type", "object" } };
	desc.invoke = [&runs](const cmd::call&) {
		++runs;
		return cmd::result::success();
	};
	cmd::registration registration = commands.add(std::move(desc));

	auto access = std::make_unique<edt::agent_access>(commands, dir.path() / "project.json", dir.path() / "personal.json");
	access->set_questions_routed(true);
	access->set_person_present(true);
	auto bridge = std::make_unique<edt::mcp_bridge_host>(commands, tasks, *access);

	std::vector<mcp::call_result> answers;
	mcp::request_context context;
	context.session_id = "s1";
	bridge->call_tool(context, "code_create", json_of("{}"), nullptr, [&answers](mcp::call_result r) { answers.push_back(std::move(r)); });
	tasks.drain_main(std::chrono::hours(1));
	BOOST_REQUIRE(answers.empty());
	BOOST_REQUIRE_EQUAL(access->confirmations().pending().size(), 1u);

	// The server stops first (the bridge goes), then the access: the question is answered "denied" on the way out.
	bridge.reset();
	access.reset();

	BOOST_REQUIRE_EQUAL(answers.size(), 1u);
	BOOST_TEST(answers[0].is_error);
	BOOST_TEST(runs == 0);
	BOOST_CHECK_NO_THROW(tasks.drain_main(std::chrono::hours(1)));
}

BOOST_AUTO_TEST_CASE(ABridgeGoneBeforeThePersonAnswers_StillDeliversTheAnswerOnce)
{
	test_util::temp_dir dir{ "edt_mcp_bridge_teardown2" };
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "code.create";
	desc.category = cmd::agent_category::code_files;
	desc.mutates = true;
	desc.args_schema = boost::json::value{ { "type", "object" } };
	desc.invoke = [&runs](const cmd::call&) {
		++runs;
		return cmd::result::success();
	};
	cmd::registration registration = commands.add(std::move(desc));

	edt::agent_access access(commands, dir.path() / "project.json", dir.path() / "personal.json");
	access.set_questions_routed(true);
	access.set_person_present(true);
	auto bridge = std::make_unique<edt::mcp_bridge_host>(commands, tasks, access);
	std::vector<mcp::call_result> answers;
	mcp::request_context context;
	context.session_id = "s1";
	bridge->call_tool(context, "code_create", json_of("{}"), nullptr, [&answers](mcp::call_result r) { answers.push_back(std::move(r)); });
	tasks.drain_main(std::chrono::hours(1));
	bridge.reset();

	const auto waiting = access.confirmations().pending();
	BOOST_REQUIRE_EQUAL(waiting.size(), 1u);
	BOOST_TEST(access.confirmations().answer(waiting[0].id, edt::confirmation_answer::allow_once));
	BOOST_REQUIRE_EQUAL(answers.size(), 1u);
	BOOST_TEST(!answers[0].is_error);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_SUITE_END()
