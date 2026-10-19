#include "edt_mcp_bridge_host.h"

#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>

#include <array>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	// The domains (the part of a command id before the first dot) whose commands only read. A6a exposes nothing else;
	// the query commands of A4 (scene, node, component, asset, selection, problems) are listed so they appear
	// the day they are registered, provided their metadata says they do not mutate.
	constexpr std::array<std::string_view, 10> read_domains = {
		"log", "job", "dialog", "viewport", "scene", "node", "component", "asset", "selection", "problems" };

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
				result.content.push_back(mcp::content_block::make_image(text_of(png->as_string()), "image/png"));
				object.erase("png_base64");
				result.content.push_back(mcp::content_block::make_text(json::serialize(value)));
			}
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
		std::future<cmd::result> outcome = state->queue->call_now_or_post(id, args, cmd::origin::agent(session));
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
		std::future<cmd::result> outcome = state->queue->call_now_or_post(id, json::parse(def->args), cmd::origin::agent(session));
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
