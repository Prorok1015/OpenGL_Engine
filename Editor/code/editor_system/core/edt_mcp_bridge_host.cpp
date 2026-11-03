#include "edt_mcp_bridge_host.h"
#include "edt_agent_access.h"

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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
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

	bool is_compare(std::string_view command_id)
	{
		return command_id == "capture.compare";
	}

	// The agent may ask for less than a ceiling, never for more.
	void ceiling(json::object& object, const char* key, std::int64_t limit)
	{
		std::int64_t value = limit;
		if (const json::value* given = object.if_contains(key)) {
			if (const std::optional<std::int64_t> number = cmd::json_schema::integer_of(*given)) {
				value = std::min(*number, limit);
			}
		}
		object[key] = value;
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
		ceiling(object, "max_side", agent_max_capture_side);
		ceiling(object, "max_pixels", agent_max_capture_pixels);
		ceiling(object, "max_source_pixels", agent_max_source_pixels);
		return std::nullopt;
	}

	// capture.compare reads the view back whole and cannot scale it (the comparison is at full size), so of the
	// viewport.capture limits only the source one applies: a bigger view, or reference, is refused.
	void limit_compare_args(json::value& args)
	{
		if (args.is_object()) {
			ceiling(args.as_object(), "max_source_pixels", agent_max_source_pixels);
		}
	}

	std::string too_large_text(std::size_t bytes)
	{
		return "the result is too large to send (" + std::to_string(bytes) + " bytes, the limit is "
			+ std::to_string(agent_max_response_bytes) + "); ask for less (a smaller size, a smaller page)";
	}

	std::string text_of(const json::string& text)
	{
		return std::string(text.data(), text.size());
	}

	const cmd::command_desc* find_tool(const cmd::command_registry& registry, edt::agent_policy& policy, std::string_view name)
	{
		for (const cmd::command_desc* command : registry.list()) {
			if (edt::agent_tool_name(*command) == name && edt::agent_may_use(*command, policy)) {
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

	// A command that needs a display in a headless editor: every call is refused by the registry with this code.
	bool unavailable_headless(const cmd::command_desc& command, const cmd::command_registry& registry)
	{
		return command.needs_display && registry.headless();
	}

	mcp::tool_info describe(const cmd::command_desc& command, const cmd::command_registry& registry)
	{
		mcp::tool_info tool;
		tool.name = edt::agent_tool_name(command);
		tool.description = command.summary;
		tool.input_schema = command.args_schema;
		if (!has_png_field(command.result_schema)) {
			tool.output_schema = command.result_schema;
		}
		tool.read_only = !command.mutates;
		// MCP reads a missing destructiveHint as true for a tool that writes: say it where the command is not risky.
		tool.destructive = command.mutates && (command.destructive || command.risk != cmd::danger::none);
		tool.idempotent = false;
		tool.open_world = false;
		tool.unavailable_headless = unavailable_headless(command, registry);
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

	// A resource is a read the client cannot be asked about: only a command that reads and that the policy simply allows.
	bool resource_readable(const cmd::command_desc& command, edt::agent_policy& policy)
	{
		return command.category == cmd::agent_category::read && !command.mutates && edt::agent_may_use(command, policy)
			&& policy.effective(command).mode == cmd::policy_mode::allow;
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

bool edt::agent_may_use(const cmd::command_desc& command, agent_policy& policy)
{
	if (!command.expose_as_tool) {
		return false;
	}
	// Whatever the policy files say, a command nobody classified, or one that is nothing for an agent, does not exist for it.
	if (command.category == cmd::agent_category::unset || command.category == cmd::agent_category::not_for_agents) {
		return false;
	}
	return policy.effective(command).mode != cmd::policy_mode::deny;
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
	agent_access* access = nullptr;
	session_closed_hook on_session_closed;
};

edt::mcp_bridge_host::mcp_bridge_host(cmd::command_registry& commands, ds::task_system& tasks, agent_access& access,
	session_closed_hook on_session_closed)
	: m_tasks(tasks)
	, m_state(std::make_shared<state>())
{
	ASSERT_MSG(tasks.on_main(), "mcp_bridge_host: create it on the main thread");
	m_state->commands = &commands;
	m_state->access = &access;
	m_state->on_session_closed = std::move(on_session_closed);
}

edt::mcp_bridge_host::~mcp_bridge_host()
{
	m_state->commands = nullptr;
	m_state->access = nullptr;
	m_state->on_session_closed = nullptr;
}

void edt::mcp_bridge_host::stop_reporting()
{
	m_state->on_session_closed = nullptr;
}

void edt::mcp_bridge_host::list_tools(std::function<void(std::vector<mcp::tool_info>)> done)
{
	m_tasks.post_main([state = m_state, done = std::move(done)] {
		std::vector<mcp::tool_info> tools;
		if (state->commands != nullptr && state->access != nullptr) {
			for (const cmd::command_desc* command : state->commands->list()) {
				if (agent_may_use(*command, state->access->policy())) {
					tools.push_back(describe(*command, *state->commands));
				}
			}
		}
		done(std::move(tools));
	});
}

void edt::mcp_bridge_host::call_tool(mcp::request_context context, std::string name, json::value args, mcp::progress_sink progress,
	std::function<void(mcp::call_result)> done)
{
	// Builds report no progress here: they return a job at once, and job.get follows it.
	(void)progress;
	m_tasks.post_main([state = m_state, session = std::move(context.session_id), name = std::move(name), args = std::move(args),
		done = std::move(done)]() mutable {
		if (state->commands == nullptr || state->access == nullptr) {
			done(error_result("the editor is shutting down"));
			return;
		}
		// The same check as the listing: a tool that is not listed does not exist for the agent.
		const cmd::command_desc* command = find_tool(*state->commands, state->access->policy(), name);
		if (command == nullptr) {
			done(error_result("unknown tool '" + name + "'"));
			return;
		}
		// Copied first: the command may unregister itself while it runs.
		const std::string id = command->id;
		json::value limited = std::move(args);
		if (is_capture(id)) {
			if (const std::optional<std::string> refusal = limit_capture_args(limited)) {
				done(error_result(*refusal));
				return;
			}
		} else if (is_compare(id)) {
			limit_compare_args(limited);
		}
		cmd::call request;
		request.args = std::move(limited);
		request.from = cmd::origin::agent(session);
		// `done` may be called later (a person is asked, a gesture is waited out). The closure holds nothing of the
		// bridge, only the answer to the client.
		state->access->confirmations().invoke(id, std::move(request),
			[done = std::move(done)](cmd::result outcome) { done(to_call_result(std::move(outcome))); });
	});
}

void edt::mcp_bridge_host::list_resources(std::function<void(std::vector<mcp::resource_info>)> done)
{
	m_tasks.post_main([state = m_state, done = std::move(done)] {
		std::vector<mcp::resource_info> resources;
		if (state->commands != nullptr && state->access != nullptr) {
			for (const resource_def& def : resource_defs) {
				const cmd::command_desc* command = state->commands->find(def.command);
				if (command == nullptr || !resource_readable(*command, state->access->policy())) {
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
	m_tasks.post_main([state = m_state, session = std::move(context.session_id), uri = std::move(uri), done = std::move(done)]() mutable {
		if (state->commands == nullptr || state->access == nullptr) {
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
		if (command == nullptr || !resource_readable(*command, state->access->policy())) {
			done(unavailable("no such resource: " + uri));
			return;
		}
		if (unavailable_headless(*command, *state->commands)) {
			done(unavailable(std::string(cmd::codes::unavailable_headless) + ": " + uri + " needs a display; this editor runs headless"));
			return;
		}
		const std::string id = command->id;
		json::value resource_args = json::parse(def->args);
		if (is_capture(id)) {
			limit_capture_args(resource_args);
		}
		cmd::call request;
		request.args = std::move(resource_args);
		request.from = cmd::origin::agent(session);
		const std::string_view mime_type = def->mime_type;
		state->access->confirmations().invoke(id, std::move(request),
			[done = std::move(done), uri = std::move(uri), mime_type](cmd::result result) {
				if (!result.ok) {
					done(unavailable(result.code + ": " + result.error));
					return;
				}
				mcp::resource_content content;
				content.uri = uri;
				content.mime_type = std::string(mime_type);
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
	});
}

void edt::mcp_bridge_host::session_closed(std::string session_id)
{
	m_tasks.post_main([state = m_state, session = std::move(session_id)] {
		if (state->on_session_closed) {
			state->on_session_closed(session);
		}
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
