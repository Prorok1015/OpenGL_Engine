#include "edt_mcp_bridge_host.h"

#include <snk/command/cmd_json_schema.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	// The domains (the part of a command id before the first dot) whose commands only read, and that exist today.
	// `mutates` defaults to false for a command, so a domain that is listed here before its commands exist would
	// expose whatever gets registered under it, flag or no flag.
	constexpr std::array<std::string_view, 4> read_domains = { "log", "job", "dialog", "viewport" };

	// The state queries of A4, problems.list, build.settings.get and the git reads (#454) are allowed one command at a time, not by
	// domain: scene.* and node.* will also hold the mutating commands of A5, vcs.* the git writes (#455), and a new command in these
	// domains stays hidden from an agent until it is reviewed and listed here.
	constexpr std::array<std::string_view, 12> read_commands = { "scene.query", "node.get", "component.schema", "asset.query", "selection.get",
		"problems.list", "build.settings.get", "vcs.info", "vcs.status", "vcs.diff", "vcs.log", "vcs.branches" };

	// What an agent may ask of viewport.capture. A request for 8192 x 8192 is a quarter of a gigabyte of readback
	// and copies; the answer is a PNG that has to be base64 encoded and sent.
	constexpr std::int64_t agent_max_capture_side = 2048;
	constexpr std::int64_t agent_max_capture_pixels = agent_max_capture_side * agent_max_capture_side;
	// The view is read back whole before it is scaled down to that, on the main thread: a view of 8 Mpixels (4K UHD
	// fits) is 32 MiB, plus the flipped copy. A bigger view is refused for an agent instead of read.
	constexpr std::int64_t agent_max_source_pixels = 8LL * 1024 * 1024;
	// Width asked when the agent names no size (the design's 1280 pixels on the long side, for a landscape view).
	constexpr std::int64_t agent_default_capture_width = 1280;
	// Largest answer sent to an agent, in bytes of JSON or base64 text.
	constexpr std::size_t agent_max_response_bytes = 16u * 1024u * 1024u;

	bool is_capture(std::string_view command_id)
	{
		return command_id == "viewport.capture";
	}

	// Limits the size an agent asks of viewport.capture. A size that is not an integer is left for the schema check of
	// the registry to refuse. Returns the reason when the request is too big.
	// The side the agent does not name is derived by the command from the aspect of the view (a tall view and
	// width=2048 would be 2048 x 8192), so the limits go into the request too (max_side, max_pixels): the command
	// refuses a derived side or a pixel count over them before it reads the view back.
	std::optional<std::string> limit_capture_args(json::value& args)
	{
		if (!args.is_object()) {
			return std::nullopt;
		}
		json::object& object = args.as_object();
		bool sized = false;
		for (const char* side : { "width", "height" }) {
			const json::value* value = object.if_contains(side);
			if (value == nullptr) {
				continue;
			}
			sized = true;
			const std::optional<std::int64_t> number = cmd::json_schema::integer_of(*value);
			if (number && *number > agent_max_capture_side) {
				return std::string(side) + " is limited to " + std::to_string(agent_max_capture_side) + " pixels for an agent";
			}
		}
		if (!sized) {
			object["width"] = agent_default_capture_width;
		}
		// The agent may ask for less than the ceilings, never for more.
		const auto ceiling = [&object](const char* key, std::int64_t limit) {
			std::int64_t value = limit;
			if (const json::value* given = object.if_contains(key)) {
				if (const std::optional<std::int64_t> number = cmd::json_schema::integer_of(*given)) {
					value = std::min(*number, limit);
				}
			}
			object[key] = value;
		};
		ceiling("max_side", agent_max_capture_side);
		ceiling("max_pixels", agent_max_capture_pixels);
		ceiling("max_source_pixels", agent_max_source_pixels);
		return std::nullopt;
	}

	std::string too_large_text(std::size_t bytes)
	{
		return "the result is too large to send (" + std::to_string(bytes) + " bytes, the limit is "
			+ std::to_string(agent_max_response_bytes) + "); ask for less (a smaller size, a smaller page)";
	}

	std::string_view domain_of(std::string_view id)
	{
		return id.substr(0, id.find('.'));
	}

	std::string text_of(const json::string& text)
	{
		return std::string(text.data(), text.size());
	}

	const cmd::command_desc* find_tool(const cmd::command_registry& registry, std::string_view name)
	{
		for (const cmd::command_desc* command : registry.list()) {
			if (edt::agent_may_use(*command) && edt::agent_tool_name(*command) == name) {
				return command;
			}
		}
		return nullptr;
	}

	// A result that carries a PNG as base64 text (viewport.capture) goes to the client as an image block, not as a
	// megabyte string inside the JSON; its tool declares no outputSchema because the structured part lacks the field.
	bool has_png_field(const json::value& schema)
	{
		if (!schema.is_object()) {
			return false;
		}
		const json::value* properties = schema.as_object().if_contains("properties");
		return properties != nullptr && properties->is_object() && properties->as_object().contains("png_base64");
	}

	mcp::tool_info describe(const cmd::command_desc& command)
	{
		mcp::tool_info tool;
		tool.name = edt::agent_tool_name(command);
		tool.description = command.summary;
		tool.input_schema = command.args_schema;
		if (!has_png_field(command.result_schema)) {
			tool.output_schema = command.result_schema;
		}
		tool.read_only = true;
		tool.destructive = false;
		return tool;
	}

	mcp::call_result error_result(std::string text)
	{
		mcp::call_result result;
		result.is_error = true;
		result.content.push_back(mcp::content_block::make_text(std::move(text)));
		return result;
	}

	mcp::call_result to_call_result(cmd::result outcome)
	{
		if (!outcome.ok) {
			return error_result(outcome.code + ": " + outcome.error);
		}
		mcp::call_result result;
		json::value value = std::move(outcome.value);
		if (value.is_object()) {
			json::object& object = value.as_object();
			const json::value* png = object.if_contains("png_base64");
			if (png != nullptr && png->is_string()) {
				if (png->as_string().size() > agent_max_response_bytes) {
					return error_result(too_large_text(png->as_string().size()));
				}
				result.content.push_back(mcp::content_block::make_image(text_of(png->as_string()), "image/png"));
				object.erase("png_base64");
				result.content.push_back(mcp::content_block::make_text(json::serialize(value)));
				result.structured = std::move(value);
				return result;
			}
		}
		const std::size_t size = json::serialize(value).size();
		if (size > agent_max_response_bytes) {
			return error_result(too_large_text(size));
		}
		result.structured = std::move(value);
		return result;
	}

	// The resources the editor offers, each read by one command.
	struct resource_def
	{
		std::string_view uri;
		std::string_view name;
		std::string_view description;
		std::string_view mime_type;
		std::string_view command;
		std::string_view args;
	};

	constexpr std::array<resource_def, 5> resource_defs = { {
		{ "snk://log", "Engine log", "The latest entries of the engine log (log.read)", "application/json", "log.read", "{}" },
		{ "snk://jobs", "Background jobs", "The running background jobs (job.list)", "application/json", "job.list", "{}" },
		{ "snk://dialogs", "Open dialogs", "The dialogs waiting for an answer (dialog.list)", "application/json", "dialog.list", "{}" },
		{ "snk://viewport/scene.png", "Scene view", "Screenshot of the scene view", "image/png", "viewport.capture", R"({"viewport":"scene"})" },
		{ "snk://viewport/game.png", "Game view", "Screenshot of the game view", "image/png", "viewport.capture", R"({"viewport":"game"})" },
	} };

	mcp::resource_result unavailable(std::string why)
	{
		mcp::resource_result result;
		result.found = false;
		result.error = std::move(why);
		return result;
	}
}

bool edt::agent_may_use(const cmd::command_desc& command)
{
	if (!command.expose_as_tool || command.mutates || command.long_running || command.risk != cmd::danger::none) {
		return false;
	}
	for (const std::string_view allowed : read_commands) {
		if (command.id == allowed) {
			return true;
		}
	}
	const std::string_view domain = domain_of(command.id);
	for (const std::string_view allowed : read_domains) {
		if (domain == allowed) {
			return true;
		}
	}
	return false;
}

std::string edt::agent_tool_name(const cmd::command_desc& command)
{
	std::string name = command.id;
	for (char& c : name) {
		if (c == '.') {
			c = '_';
		}
	}
	if (command.version > 1) {
		name += "_v" + std::to_string(command.version);
	}
	return name;
}

// Read and written on the main thread only: the destructor and the posted calls both run there, so a call that is
// still queued when the host goes sees null pointers and answers with an error.
struct edt::mcp_bridge_host::state
{
	cmd::command_registry* commands = nullptr;
	cmd::main_thread_queue* queue = nullptr;
};

edt::mcp_bridge_host::mcp_bridge_host(cmd::command_registry& commands, cmd::main_thread_queue& queue, ds::task_system& tasks)
	: m_tasks(tasks)
	, m_state(std::make_shared<state>())
{
	ASSERT_MSG(tasks.on_main(), "mcp_bridge_host: create it on the main thread");
	m_state->commands = &commands;
	m_state->queue = &queue;
}

edt::mcp_bridge_host::~mcp_bridge_host()
{
	m_state->commands = nullptr;
	m_state->queue = nullptr;
}

void edt::mcp_bridge_host::list_tools(std::function<void(std::vector<mcp::tool_info>)> done)
{
	m_tasks.post_main([state = m_state, done = std::move(done)] {
		std::vector<mcp::tool_info> tools;
		if (state->commands != nullptr) {
			for (const cmd::command_desc* command : state->commands->list()) {
				if (agent_may_use(*command)) {
					tools.push_back(describe(*command));
				}
			}
		}
		done(std::move(tools));
	});
}

void edt::mcp_bridge_host::call_tool(mcp::request_context context, std::string name, json::value args, mcp::progress_sink progress,
	std::function<void(mcp::call_result)> done)
{
	// A6a has no long-running tool, so there is no progress to report.
	(void)progress;
	m_tasks.post_main([state = m_state, session = std::move(context.session_id), name = std::move(name), args = std::move(args),
		done = std::move(done)] {
		if (state->commands == nullptr || state->queue == nullptr) {
			done(error_result("the editor is shutting down"));
			return;
		}
		// The same check as the listing: a tool that is not listed does not exist for the agent.
		const cmd::command_desc* command = find_tool(*state->commands, name);
		if (command == nullptr) {
			done(error_result("unknown tool '" + name + "'"));
			return;
		}
		// Copied first: the command may unregister itself while it runs.
		const std::string id = command->id;
		json::value limited = args;
		if (is_capture(id)) {
			if (const std::optional<std::string> refusal = limit_capture_args(limited)) {
				done(error_result(*refusal));
				return;
			}
		}
		std::future<cmd::result> outcome = state->queue->call_now_or_post(id, std::move(limited), cmd::origin::agent(session));
		// Ready: this runs on the main thread, where call_now_or_post runs the command inline.
		done(to_call_result(outcome.get()));
	});
}

void edt::mcp_bridge_host::list_resources(std::function<void(std::vector<mcp::resource_info>)> done)
{
	m_tasks.post_main([state = m_state, done = std::move(done)] {
		std::vector<mcp::resource_info> resources;
		if (state->commands != nullptr) {
			for (const resource_def& def : resource_defs) {
				const cmd::command_desc* command = state->commands->find(def.command);
				if (command == nullptr || !agent_may_use(*command)) {
					continue;
				}
				mcp::resource_info info;
				info.uri = std::string(def.uri);
				info.name = std::string(def.name);
				info.description = std::string(def.description);
				info.mime_type = std::string(def.mime_type);
				resources.push_back(std::move(info));
			}
		}
		done(std::move(resources));
	});
}

void edt::mcp_bridge_host::read_resource(mcp::request_context context, std::string uri, std::function<void(mcp::resource_result)> done)
{
	m_tasks.post_main([state = m_state, session = std::move(context.session_id), uri = std::move(uri), done = std::move(done)] {
		if (state->commands == nullptr || state->queue == nullptr) {
			done(unavailable("the editor is shutting down"));
			return;
		}
		const resource_def* def = nullptr;
		for (const resource_def& candidate : resource_defs) {
			if (candidate.uri == uri) {
				def = &candidate;
			}
		}
		const cmd::command_desc* command = def != nullptr ? state->commands->find(def->command) : nullptr;
		if (command == nullptr || !agent_may_use(*command)) {
			done(unavailable("no such resource: " + uri));
			return;
		}
		const std::string id = command->id;
		json::value resource_args = json::parse(def->args);
		if (is_capture(id)) {
			limit_capture_args(resource_args);
		}
		std::future<cmd::result> outcome = state->queue->call_now_or_post(id, std::move(resource_args), cmd::origin::agent(session));
		cmd::result result = outcome.get();
		if (!result.ok) {
			done(unavailable(result.code + ": " + result.error));
			return;
		}

		mcp::resource_content content;
		content.uri = uri;
		content.mime_type = std::string(def->mime_type);
		const json::value* png = result.value.is_object() ? result.value.as_object().if_contains("png_base64") : nullptr;
		if (png != nullptr && png->is_string()) {
			content.is_blob = true;
			content.blob = text_of(png->as_string());
		} else {
			content.text = json::serialize(result.value);
		}
		if (content.blob.size() + content.text.size() > agent_max_response_bytes) {
			done(unavailable(too_large_text(content.blob.size() + content.text.size())));
			return;
		}
		mcp::resource_result found;
		found.found = true;
		found.contents.push_back(std::move(content));
		done(std::move(found));
	});
}

void edt::mcp_bridge_host::log(mcp::log_level level, std::string_view message)
{
	const std::string text(message);
	switch (level) {
	case mcp::log_level::debug: egLOG_DEBUG("mcp", "{}", text); break;
	case mcp::log_level::info: egLOG_INFO("mcp", "{}", text); break;
	case mcp::log_level::warn: egLOG_WARN("mcp", "{}", text); break;
	case mcp::log_level::error: egLOG_ERROR("mcp", "{}", text); break;
	}
}
