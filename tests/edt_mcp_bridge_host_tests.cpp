#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "edt_mcp_bridge_host.h"
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// edt::mcp_bridge_host: the editor's side of the MCP server. Which commands an agent sees (read-only, fail closed),
// and that every call hops to the main thread. The task system is manual here, so the test is the frame loop and the
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

struct bridge_fixture
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	cmd::main_thread_queue queue{ commands, tasks };
	edt::mcp_bridge_host bridge{ commands, queue, tasks };
	std::vector<cmd::registration> registrations;

	std::size_t drain()
	{
		return tasks.drain_main(std::chrono::hours(1));
	}

	cmd::command_desc make(std::string id)
	{
		cmd::command_desc desc;
		desc.id = std::move(id);
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

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyTests)

BOOST_AUTO_TEST_CASE(ReadCommandsInAReadDomain_AreAllowed)
{
	cmd::command_desc desc;
	for (const char* id : { "log.read", "job.list", "job.get", "dialog.list", "viewport.capture", "scene.query", "node.get", "selection.get" }) {
		desc.id = id;
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST(edt::agent_may_use(desc));
		}
	}
}

BOOST_AUTO_TEST_CASE(MutatingRiskyLongRunningAndHiddenCommands_AreNot)
{
	cmd::command_desc desc;
	desc.id = "job.cancel";
	desc.mutates = true;
	BOOST_TEST(!edt::agent_may_use(desc));

	desc = {};
	desc.id = "dialog.answer";
	desc.risk = cmd::danger::confirm;
	BOOST_TEST(!edt::agent_may_use(desc));
	desc.risk = cmd::danger::deny_by_default;
	BOOST_TEST(!edt::agent_may_use(desc));

	desc = {};
	desc.id = "scene.import";
	desc.long_running = true;
	BOOST_TEST(!edt::agent_may_use(desc));

	desc = {};
	desc.id = "log.read";
	desc.expose_as_tool = false;
	BOOST_TEST(!edt::agent_may_use(desc));
}

BOOST_AUTO_TEST_CASE(ADomainOutsideTheReadList_IsNotAllowedEvenIfTheMetadataSaysRead)
{
	// The metadata of file.* and app.* does not say mutates, yet they save, quit and stop time: only a known read
	// domain is exposed.
	cmd::command_desc desc;
	for (const char* id : { "file.save", "file.exit", "file.new", "app.advance", "app.resume", "help.about", "edit.undo", "nodot", "" }) {
		desc.id = id;
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST(!edt::agent_may_use(desc));
		}
	}
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

BOOST_AUTO_TEST_CASE(ListTools_ExposesTheReadCommandsOnly)
{
	cmd::command_desc read = make("log.read");
	read.args_schema = json_of(R"({"type":"object","properties":{"limit":{"type":"integer"}},"additionalProperties":false})");
	read.result_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
	add(std::move(read));
	cmd::command_desc mutating = make("job.cancel");
	mutating.mutates = true;
	add(std::move(mutating));
	add(make("file.save"));
	add(make("app.advance"));
	cmd::command_desc hidden = make("dialog.list");
	hidden.expose_as_tool = false;
	add(std::move(hidden));
	add(make("job.list"));

	const std::vector<mcp::tool_info> tools = list_tools();
	BOOST_TEST((names_of(tools) == std::vector<std::string>{ "log_read", "job_list" }));
	BOOST_TEST(tools[0].description == "A command");
	BOOST_TEST(tools[0].read_only);
	BOOST_TEST(!tools[0].destructive);
	BOOST_TEST(json::serialize(tools[0].input_schema) == R"({"type":"object","properties":{"limit":{"type":"integer"}},"additionalProperties":false})");
	BOOST_TEST(tools[0].output_schema.is_object());
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
	for (const char* id : { "job.cancel", "file.save", "app.advance" }) {
		cmd::command_desc desc = make(id);
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

BOOST_AUTO_TEST_CASE(HostGoneBeforeTheDrain_AnswersWithAnError)
{
	add(make("log.read"));
	auto doomed = std::make_unique<edt::mcp_bridge_host>(commands, queue, tasks);
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
	capture.args_schema = json_of(R"({"type":"object","properties":{"viewport":{"type":"string"}},"additionalProperties":false})");
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
	capture.args_schema = json_of(R"({"type":"object","properties":{"viewport":{"type":"string"}},"additionalProperties":false})");
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
