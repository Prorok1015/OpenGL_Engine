#include <snk/mcp/mcp_rpc.h>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	constexpr std::array<std::string_view, 2> supported_protocols = { "2025-06-18", "2025-03-26" };

	json::string make_string(std::string_view text)
	{
		return json::string(text);
	}

	std::string string_of(const json::string& text)
	{
		return std::string(text.c_str(), text.size());
	}

	bool is_id(const json::value& value)
	{
		return value.is_string() || value.is_int64() || value.is_uint64();
	}

	json::value tool_json(mcp::tool_info& tool)
	{
		json::object entry;
		entry["name"] = make_string(tool.name);
		entry["description"] = make_string(tool.unavailable_headless ? tool.description + " (unavailable in headless mode)" : tool.description);
		if (tool.input_schema.is_object()) {
			entry["inputSchema"] = std::move(tool.input_schema);
		} else {
			json::object schema;
			schema["type"] = "object";
			entry["inputSchema"] = std::move(schema);
		}
		if (tool.output_schema.is_object()) {
			entry["outputSchema"] = std::move(tool.output_schema);
		}
		json::object annotations;
		annotations["readOnlyHint"] = tool.read_only;
		annotations["destructiveHint"] = tool.destructive;
		annotations["idempotentHint"] = tool.idempotent;
		annotations["openWorldHint"] = tool.open_world;
		entry["annotations"] = std::move(annotations);
		if (tool.unavailable_headless) {
			json::object meta;
			meta["snk/unavailableHeadless"] = true;
			entry["_meta"] = std::move(meta);
		}
		return entry;
	}

	json::value call_result_json(mcp::call_result& result)
	{
		json::array content;
		content.reserve(result.content.size() + 1);
		for (mcp::content_block& block : result.content) {
			json::object entry;
			if (block.type == mcp::content_block::kind::image) {
				entry["type"] = "image";
				entry["data"] = make_string(block.data);
				entry["mimeType"] = make_string(block.mime_type);
			} else {
				entry["type"] = "text";
				entry["text"] = make_string(block.text);
			}
			content.push_back(std::move(entry));
		}
		const bool has_structured = result.structured.is_object();
		if (content.empty() && has_structured) {
			// A client that predates structuredContent still gets the data.
			json::object entry;
			entry["type"] = "text";
			entry["text"] = make_string(json::serialize(result.structured));
			content.push_back(std::move(entry));
		}
		json::object out;
		out["content"] = std::move(content);
		if (has_structured) {
			out["structuredContent"] = std::move(result.structured);
		}
		out["isError"] = result.is_error;
		return out;
	}

	json::value resource_json(const mcp::resource_info& resource)
	{
		json::object entry;
		entry["uri"] = make_string(resource.uri);
		entry["name"] = make_string(resource.name);
		if (!resource.description.empty()) {
			entry["description"] = make_string(resource.description);
		}
		if (!resource.mime_type.empty()) {
			entry["mimeType"] = make_string(resource.mime_type);
		}
		return entry;
	}

	json::value resource_content_json(const mcp::resource_content& content)
	{
		json::object entry;
		entry["uri"] = make_string(content.uri);
		if (!content.mime_type.empty()) {
			entry["mimeType"] = make_string(content.mime_type);
		}
		if (content.is_blob) {
			entry["blob"] = make_string(content.blob);
		} else {
			entry["text"] = make_string(content.text);
		}
		return entry;
	}
}

bool mcp::is_supported_protocol(std::string_view version)
{
	return std::find(supported_protocols.begin(), supported_protocols.end(), version) != supported_protocols.end();
}

json::value mcp::rpc_result(const json::value& id, json::value result)
{
	json::object message;
	message["jsonrpc"] = "2.0";
	message["id"] = id;
	message["result"] = std::move(result);
	return message;
}

json::value mcp::rpc_error(const json::value& id, int code, std::string_view text)
{
	json::object error;
	error["code"] = code;
	error["message"] = make_string(text);
	json::object message;
	message["jsonrpc"] = "2.0";
	message["id"] = id;
	message["error"] = std::move(error);
	return message;
}

json::value mcp::rpc_notification(std::string_view method, json::value params)
{
	json::object message;
	message["jsonrpc"] = "2.0";
	message["method"] = make_string(method);
	if (!params.is_null()) {
		message["params"] = std::move(params);
	}
	return message;
}

std::string mcp::new_session_id()
{
	static constexpr char digits[] = "0123456789abcdef";
	constexpr std::size_t bytes = 16;
	std::random_device source;
	std::string out;
	out.reserve(bytes * 2);
	for (std::size_t i = 0; i < bytes; ++i) {
		const unsigned value = source() & 0xFFu;
		out.push_back(digits[value >> 4]);
		out.push_back(digits[value & 0xFu]);
	}
	return out;
}

mcp::rpc_message mcp::classify_rpc(const json::object& message)
{
	rpc_message out;
	const json::value* method_value = message.if_contains("method");
	const json::value* id_value = message.if_contains("id");
	if (method_value == nullptr) {
		// A response to a request of ours; the server sends none, so there is nothing to match it with.
		if (id_value != nullptr && (message.contains("result") || message.contains("error"))) {
			out.type = rpc_message::kind::response;
			return out;
		}
		out.error = "a JSON-RPC message needs a method";
		return out;
	}
	if (!method_value->is_string()) {
		out.error = "method must be a string";
		return out;
	}
	const bool is_request = id_value != nullptr;
	if (is_request && !is_id(*id_value)) {
		out.error = "id must be a string or an integer";
		return out;
	}
	out.type = is_request ? rpc_message::kind::request : rpc_message::kind::notification;
	out.method = string_of(method_value->get_string());
	if (is_request) {
		out.id = *id_value;
	}
	if (const json::value* params_value = message.if_contains("params")) {
		if (params_value->is_object()) {
			out.params = params_value->get_object();
		} else {
			out.bad_params = true;
		}
	}
	return out;
}

mcp::rpc_dispatcher::rpc_dispatcher(host& application, server_info info)
	: m_host(application)
	, m_info(std::move(info))
{
}

json::value mcp::rpc_dispatcher::initialize(const json::value& id, const json::object& params, bool& accepted) const
{
	accepted = false;
	const json::value* requested = params.if_contains("protocolVersion");
	if (requested == nullptr || !requested->is_string()) {
		return rpc_error(id, rpc_codes::invalid_params, "protocolVersion is required");
	}
	const std::string asked = string_of(requested->get_string());
	const std::string negotiated = is_supported_protocol(asked) ? asked : std::string(latest_protocol_version);

	json::object tools;
	tools["listChanged"] = true;
	json::object resources;
	resources["subscribe"] = true;
	resources["listChanged"] = false;
	json::object capabilities;
	capabilities["tools"] = std::move(tools);
	capabilities["resources"] = std::move(resources);

	json::object info;
	info["name"] = make_string(m_info.name);
	info["version"] = make_string(m_info.version);
	if (!m_info.snk_api.empty()) {
		info["snk_api"] = make_string(m_info.snk_api);
	}

	json::object result;
	result["protocolVersion"] = make_string(negotiated);
	result["capabilities"] = std::move(capabilities);
	result["serverInfo"] = std::move(info);
	if (!m_info.instructions.empty()) {
		result["instructions"] = make_string(m_info.instructions);
	}
	accepted = true;
	return rpc_result(id, std::move(result));
}

void mcp::rpc_dispatcher::dispatch(const std::string& method, const json::value& id, const json::object& params,
	const rpc_session& session, const reply_sink& reply) const
{
	if (method == "ping") {
		reply(rpc_result(id, json::object{}));
	} else if (method == "tools/list") {
		m_host.list_tools([reply, id](std::vector<tool_info> tools) {
			json::array list;
			list.reserve(tools.size());
			for (tool_info& tool : tools) {
				list.push_back(tool_json(tool));
			}
			json::object result;
			result["tools"] = std::move(list);
			reply(rpc_result(id, std::move(result)));
		});
	} else if (method == "tools/call") {
		const json::value* name = params.if_contains("name");
		if (name == nullptr || !name->is_string()) {
			reply(rpc_error(id, rpc_codes::invalid_params, "name is required"));
			return;
		}
		json::value arguments = json::object{};
		if (const json::value* given = params.if_contains("arguments")) {
			if (given->is_object()) {
				arguments = *given;
			} else if (!given->is_null()) {
				reply(rpc_error(id, rpc_codes::invalid_params, "arguments must be an object"));
				return;
			}
		}
		progress_sink progress = [](double, double, std::string_view) {};
		if (session.notify) {
			const json::value* meta = params.if_contains("_meta");
			const json::value* token = (meta != nullptr && meta->is_object()) ? meta->get_object().if_contains("progressToken") : nullptr;
			if (token != nullptr && is_id(*token)) {
				progress = [notify = session.notify, token_copy = *token](double done_units, double total, std::string_view text) {
					json::object body;
					body["progressToken"] = token_copy;
					body["progress"] = done_units;
					if (total > 0) {
						body["total"] = total;
					}
					if (!text.empty()) {
						body["message"] = make_string(text);
					}
					notify(rpc_notification("notifications/progress", std::move(body)));
				};
			}
		}
		request_context context;
		context.session_id = session.id;
		m_host.call_tool(std::move(context), string_of(name->get_string()), std::move(arguments), std::move(progress),
			[reply, id](call_result result) {
				reply(rpc_result(id, call_result_json(result)));
			});
	} else if (method == "resources/list") {
		m_host.list_resources([reply, id](std::vector<resource_info> resources) {
			json::array list;
			list.reserve(resources.size());
			for (const resource_info& resource : resources) {
				list.push_back(resource_json(resource));
			}
			json::object result;
			result["resources"] = std::move(list);
			reply(rpc_result(id, std::move(result)));
		});
	} else if (method == "resources/templates/list") {
		json::object result;
		result["resourceTemplates"] = json::array{};
		reply(rpc_result(id, std::move(result)));
	} else if (method == "resources/read") {
		const json::value* uri = params.if_contains("uri");
		if (uri == nullptr || !uri->is_string()) {
			reply(rpc_error(id, rpc_codes::invalid_params, "uri is required"));
			return;
		}
		request_context context;
		context.session_id = session.id;
		m_host.read_resource(std::move(context), string_of(uri->get_string()), [reply, id](resource_result outcome) {
			if (!outcome.found) {
				reply(rpc_error(id, rpc_codes::resource_not_found,
					outcome.error.empty() ? std::string_view("resource not found") : std::string_view(outcome.error)));
				return;
			}
			json::array contents;
			contents.reserve(outcome.contents.size());
			for (const resource_content& content : outcome.contents) {
				contents.push_back(resource_content_json(content));
			}
			json::object result;
			result["contents"] = std::move(contents);
			reply(rpc_result(id, std::move(result)));
		});
	} else if (method == "resources/subscribe" || method == "resources/unsubscribe") {
		const json::value* uri = params.if_contains("uri");
		if (uri == nullptr || !uri->is_string()) {
			reply(rpc_error(id, rpc_codes::invalid_params, "uri is required"));
			return;
		}
		std::string text = string_of(uri->get_string());
		if (method == "resources/unsubscribe") {
			if (session.subscribe) {
				session.subscribe(text, false);
			}
			reply(rpc_result(id, json::object{}));
			return;
		}
		// Only a resource the host offers can be subscribed to; the list may arrive later.
		m_host.list_resources([subscribe = session.subscribe, id, text = std::move(text), reply](std::vector<resource_info> resources) {
			const bool known = std::any_of(resources.begin(), resources.end(), [&text](const resource_info& r) { return r.uri == text; });
			if (!known) {
				reply(rpc_error(id, rpc_codes::invalid_params, "unknown resource"));
				return;
			}
			if (subscribe && !subscribe(text, true)) {
				reply(rpc_error(id, rpc_codes::invalid_params, "too many subscriptions"));
				return;
			}
			reply(rpc_result(id, json::object{}));
		});
	} else {
		reply(rpc_error(id, rpc_codes::method_not_found, "method not found"));
	}
}
