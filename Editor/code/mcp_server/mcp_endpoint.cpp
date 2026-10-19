#include <snk/mcp/mcp_endpoint.h>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	constexpr std::string_view latest_protocol = "2025-06-18";
	constexpr std::array<std::string_view, 2> supported_protocols = { "2025-06-18", "2025-03-26" };

	// JSON-RPC and MCP error codes.
	constexpr int parse_error = -32700;
	constexpr int invalid_request = -32600;
	constexpr int method_not_found = -32601;
	constexpr int invalid_params = -32602;
	constexpr int internal_error = -32603;
	constexpr int resource_not_found = -32002;

	char lower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}

	std::string lower_copy(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) {
			c = lower(c);
		}
		return out;
	}

	json::string make_string(std::string_view text)
	{
		return json::string(text);
	}

	bool is_supported(std::string_view version)
	{
		return std::find(supported_protocols.begin(), supported_protocols.end(), version) != supported_protocols.end();
	}

	// Compares without stopping at the first difference.
	bool secure_equals(std::string_view a, std::string_view b)
	{
		unsigned char diff = a.size() == b.size() ? 0 : 1;
		const std::size_t length = a.size() > b.size() ? a.size() : b.size();
		for (std::size_t i = 0; i < length; ++i) {
			const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
			const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
			diff = static_cast<unsigned char>(diff | (ca ^ cb));
		}
		return diff == 0;
	}

	json::value rpc_result(const json::value& id, json::value result)
	{
		json::object message;
		message["jsonrpc"] = "2.0";
		message["id"] = id;
		message["result"] = std::move(result);
		return message;
	}

	json::value rpc_error(const json::value& id, int code, std::string_view text)
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

	json::value notification(std::string_view method, json::value params)
	{
		json::object message;
		message["jsonrpc"] = "2.0";
		message["method"] = make_string(method);
		if (!params.is_null()) {
			message["params"] = std::move(params);
		}
		return message;
	}

	mcp::http_response json_response(const json::value& message, int status = 200)
	{
		mcp::http_response response;
		response.status = status;
		response.set_header("Content-Type", "application/json");
		response.body = json::serialize(message);
		return response;
	}

	// A refusal that never reached JSON-RPC: the error object has a null id.
	mcp::http_response error_response(int status, int code, std::string_view text, bool close = false)
	{
		mcp::http_response response = json_response(rpc_error(nullptr, code, text), status);
		response.close = close;
		return response;
	}

	mcp::http_response accepted_response()
	{
		mcp::http_response response;
		response.status = 202;
		return response;
	}

	std::string path_of(const std::string& target)
	{
		const std::size_t query = target.find('?');
		return query == std::string::npos ? target : target.substr(0, query);
	}

	std::string random_hex(std::size_t bytes)
	{
		static constexpr char digits[] = "0123456789abcdef";
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

	json::value tool_json(mcp::tool_info& tool)
	{
		json::object entry;
		entry["name"] = make_string(tool.name);
		entry["description"] = make_string(tool.description);
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
		entry["annotations"] = std::move(annotations);
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

mcp::endpoint::endpoint(host& application, endpoint_config config)
	: m_host(application)
	, m_config(std::move(config))
{
}

mcp::endpoint::~endpoint()
{
	close_all();
}

void mcp::endpoint::set_port(std::uint16_t port)
{
	m_port = port;
}

std::size_t mcp::endpoint::session_count() const
{
	std::lock_guard lock(m_mutex);
	return m_sessions.size();
}

bool mcp::endpoint::host_header_ok(const http_request& request) const
{
	const std::string* header = request.header("host");
	if (header == nullptr) {
		return false;
	}
	const std::string value = lower_copy(*header);
	const std::size_t colon = value.rfind(':');
	const std::string name = value.substr(0, colon);
	if (name != "127.0.0.1" && name != "localhost") {
		return false;
	}
	const std::uint16_t port = m_port.load();
	if (port == 0) {
		return true;
	}
	return colon != std::string::npos && value.substr(colon + 1) == std::to_string(port);
}

bool mcp::endpoint::origin_ok(const http_request& request) const
{
	const std::string* origin = request.header("origin");
	if (origin == nullptr) {
		return true;
	}
	return std::find(m_config.allowed_origins.begin(), m_config.allowed_origins.end(), *origin) != m_config.allowed_origins.end();
}

bool mcp::endpoint::token_ok(const http_request& request) const
{
	if (m_config.token.empty()) {
		return false;
	}
	const std::string* header = request.header("authorization");
	if (header == nullptr) {
		return false;
	}
	constexpr std::string_view scheme = "bearer ";
	if (header->size() <= scheme.size() || lower_copy(std::string_view(*header).substr(0, scheme.size())) != scheme) {
		return false;
	}
	return secure_equals(std::string_view(*header).substr(scheme.size()), m_config.token);
}

void mcp::endpoint::handle(const http_request& request, responder respond)
{
	auto answered = std::make_shared<std::atomic<bool>>(false);
	responder once = [respond = std::move(respond), answered](http_response response) {
		if (!answered->exchange(true)) {
			respond(std::move(response));
		}
	};
	try {
		route(request, once);
	} catch (const std::exception& e) {
		m_host.log(log_level::error, std::string("request failed: ") + e.what());
		once(error_response(500, internal_error, "internal error"));
	} catch (...) {
		once(error_response(500, internal_error, "internal error"));
	}
}

void mcp::endpoint::route(const http_request& request, const responder& respond)
{
	// DNS rebinding: a page on another origin can make the browser connect to 127.0.0.1 under its own host name.
	if (!host_header_ok(request)) {
		respond(error_response(403, invalid_request, "forbidden host", true));
		return;
	}
	if (!origin_ok(request)) {
		respond(error_response(403, invalid_request, "forbidden origin", true));
		return;
	}
	if (!token_ok(request)) {
		http_response response = error_response(401, invalid_request, "a bearer token is required", true);
		response.set_header("WWW-Authenticate", "Bearer realm=\"snk-editor\"");
		respond(std::move(response));
		return;
	}
	if (path_of(request.target) != m_config.path) {
		respond(error_response(404, invalid_request, "not found"));
		return;
	}
	if (request.method == "POST") {
		handle_post(request, respond);
	} else if (request.method == "GET") {
		handle_get(request, respond);
	} else if (request.method == "DELETE") {
		handle_delete(request, respond);
	} else {
		http_response response = error_response(405, invalid_request, "method not allowed");
		response.set_header("Allow", "POST, GET, DELETE");
		respond(std::move(response));
	}
}

bool mcp::endpoint::lookup_session(const http_request& request, std::string& session_id,
	std::shared_ptr<event_channel>& channel, http_response& error) const
{
	const std::string* header = request.header("mcp-session-id");
	if (header == nullptr || header->empty()) {
		error = error_response(400, invalid_request, "the Mcp-Session-Id header is required");
		return false;
	}
	std::lock_guard lock(m_mutex);
	const auto found = m_sessions.find(*header);
	if (found == m_sessions.end()) {
		error = error_response(404, invalid_request, "unknown session, initialize again");
		return false;
	}
	session_id = found->first;
	channel = found->second.channel;
	return true;
}

std::string mcp::endpoint::create_session()
{
	std::string id = random_hex(16);
	{
		std::lock_guard lock(m_mutex);
		// A client that never sent DELETE must not lock the others out: the oldest session makes room.
		while (!m_sessions.empty() && m_sessions.size() >= m_config.max_sessions) {
			auto oldest = m_sessions.begin();
			for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
				if (it->second.serial < oldest->second.serial) {
					oldest = it;
				}
			}
			if (oldest->second.channel) {
				oldest->second.channel->close();
			}
			m_sessions.erase(oldest);
		}
		session created;
		created.serial = m_next_serial++;
		m_sessions.emplace(id, std::move(created));
	}
	return id;
}

void mcp::endpoint::handle_post(const http_request& request, const responder& respond)
{
	const std::string* content_type = request.header("content-type");
	if (content_type == nullptr || lower_copy(*content_type).rfind("application/json", 0) != 0) {
		respond(error_response(415, invalid_request, "Content-Type must be application/json"));
		return;
	}
	if (const std::string* version = request.header("mcp-protocol-version")) {
		if (!is_supported(*version)) {
			respond(error_response(400, invalid_request, "unsupported MCP-Protocol-Version"));
			return;
		}
	}

	json::parse_options options;
	options.max_depth = 64;
	boost::system::error_code ec;
	json::value message = json::parse(request.body, ec, json::storage_ptr(), options);
	if (ec) {
		respond(error_response(400, parse_error, "parse error"));
		return;
	}
	if (message.is_array()) {
		respond(error_response(400, invalid_request, "batch requests are not supported"));
		return;
	}
	if (!message.is_object()) {
		respond(error_response(400, invalid_request, "a JSON-RPC message is an object"));
		return;
	}
	dispatch_rpc(message.get_object(), request, respond);
}

void mcp::endpoint::dispatch_rpc(const json::object& message, const http_request& request, const responder& respond)
{
	const json::value* method_value = message.if_contains("method");
	const json::value* id_value = message.if_contains("id");
	if (method_value == nullptr) {
		// A response to a request of ours; the server sends none, so there is nothing to match it with.
		if (id_value != nullptr && (message.contains("result") || message.contains("error"))) {
			respond(accepted_response());
			return;
		}
		respond(error_response(400, invalid_request, "a JSON-RPC message needs a method"));
		return;
	}
	if (!method_value->is_string()) {
		respond(error_response(400, invalid_request, "method must be a string"));
		return;
	}
	const std::string method(method_value->get_string().c_str(), method_value->get_string().size());
	const bool is_request = id_value != nullptr;
	if (is_request && !(id_value->is_string() || id_value->is_int64() || id_value->is_uint64())) {
		respond(error_response(400, invalid_request, "id must be a string or an integer"));
		return;
	}
	const json::value id = is_request ? *id_value : json::value(nullptr);

	json::object params;
	if (const json::value* params_value = message.if_contains("params")) {
		if (!params_value->is_object()) {
			if (is_request) {
				respond(json_response(rpc_error(id, invalid_params, "params must be an object")));
			} else {
				respond(accepted_response());
			}
			return;
		}
		params = params_value->get_object();
	}

	if (method == "initialize" && is_request) {
		on_initialize(id, params, respond);
		return;
	}

	std::string session_id;
	std::shared_ptr<event_channel> channel;
	http_response session_error;
	if (!lookup_session(request, session_id, channel, session_error)) {
		respond(std::move(session_error));
		return;
	}
	if (!is_request) {
		// notifications/initialized, notifications/cancelled and the rest need no answer.
		respond(accepted_response());
		return;
	}

	if (method == "ping") {
		respond(json_response(rpc_result(id, json::object{})));
	} else if (method == "tools/list") {
		m_host.list_tools([respond, id](std::vector<tool_info> tools) {
			json::array list;
			list.reserve(tools.size());
			for (tool_info& tool : tools) {
				list.push_back(tool_json(tool));
			}
			json::object result;
			result["tools"] = std::move(list);
			respond(json_response(rpc_result(id, std::move(result))));
		});
	} else if (method == "tools/call") {
		const json::value* name = params.if_contains("name");
		if (name == nullptr || !name->is_string()) {
			respond(json_response(rpc_error(id, invalid_params, "name is required")));
			return;
		}
		json::value arguments = json::object{};
		if (const json::value* given = params.if_contains("arguments")) {
			if (given->is_object()) {
				arguments = *given;
			} else if (!given->is_null()) {
				respond(json_response(rpc_error(id, invalid_params, "arguments must be an object")));
				return;
			}
		}
		progress_sink progress = [](double, double, std::string_view) {};
		if (channel) {
			const json::value* meta = params.if_contains("_meta");
			const json::value* token = (meta != nullptr && meta->is_object()) ? meta->get_object().if_contains("progressToken") : nullptr;
			if (token != nullptr && (token->is_string() || token->is_int64() || token->is_uint64())) {
				progress = [weak = std::weak_ptr<event_channel>(channel), token_copy = *token](double done_units, double total, std::string_view text) {
					const std::shared_ptr<event_channel> stream = weak.lock();
					if (!stream) {
						return;
					}
					json::object body;
					body["progressToken"] = token_copy;
					body["progress"] = done_units;
					if (total > 0) {
						body["total"] = total;
					}
					if (!text.empty()) {
						body["message"] = make_string(text);
					}
					stream->push(sse_message(json::serialize(notification("notifications/progress", std::move(body)))));
				};
			}
		}
		request_context context;
		context.session_id = session_id;
		m_host.call_tool(std::move(context), std::string(name->get_string().c_str(), name->get_string().size()),
			std::move(arguments), std::move(progress), [respond, id](call_result result) {
				respond(json_response(rpc_result(id, call_result_json(result))));
			});
	} else if (method == "resources/list") {
		m_host.list_resources([respond, id](std::vector<resource_info> resources) {
			json::array list;
			list.reserve(resources.size());
			for (const resource_info& resource : resources) {
				list.push_back(resource_json(resource));
			}
			json::object result;
			result["resources"] = std::move(list);
			respond(json_response(rpc_result(id, std::move(result))));
		});
	} else if (method == "resources/templates/list") {
		json::object result;
		result["resourceTemplates"] = json::array{};
		respond(json_response(rpc_result(id, std::move(result))));
	} else if (method == "resources/read") {
		const json::value* uri = params.if_contains("uri");
		if (uri == nullptr || !uri->is_string()) {
			respond(json_response(rpc_error(id, invalid_params, "uri is required")));
			return;
		}
		request_context context;
		context.session_id = session_id;
		m_host.read_resource(std::move(context), std::string(uri->get_string().c_str(), uri->get_string().size()),
			[respond, id](resource_result outcome) {
				if (!outcome.found) {
					respond(json_response(rpc_error(id, resource_not_found,
						outcome.error.empty() ? std::string_view("resource not found") : std::string_view(outcome.error))));
					return;
				}
				json::array contents;
				contents.reserve(outcome.contents.size());
				for (const resource_content& content : outcome.contents) {
					contents.push_back(resource_content_json(content));
				}
				json::object result;
				result["contents"] = std::move(contents);
				respond(json_response(rpc_result(id, std::move(result))));
			});
	} else if (method == "resources/subscribe" || method == "resources/unsubscribe") {
		on_subscribe(id, params, session_id, method == "resources/subscribe", respond);
	} else {
		respond(json_response(rpc_error(id, method_not_found, "method not found")));
	}
}

void mcp::endpoint::on_initialize(const json::value& id, const json::object& params, const responder& respond)
{
	const json::value* requested = params.if_contains("protocolVersion");
	if (requested == nullptr || !requested->is_string()) {
		respond(json_response(rpc_error(id, invalid_params, "protocolVersion is required")));
		return;
	}
	const std::string asked(requested->get_string().c_str(), requested->get_string().size());
	const std::string negotiated = is_supported(asked) ? asked : std::string(latest_protocol);

	json::object tools;
	tools["listChanged"] = true;
	json::object resources;
	resources["subscribe"] = true;
	resources["listChanged"] = false;
	json::object capabilities;
	capabilities["tools"] = std::move(tools);
	capabilities["resources"] = std::move(resources);

	json::object info;
	info["name"] = make_string(m_config.info.name);
	info["version"] = make_string(m_config.info.version);
	if (!m_config.info.snk_api.empty()) {
		info["snk_api"] = make_string(m_config.info.snk_api);
	}

	json::object result;
	result["protocolVersion"] = make_string(negotiated);
	result["capabilities"] = std::move(capabilities);
	result["serverInfo"] = std::move(info);
	if (!m_config.info.instructions.empty()) {
		result["instructions"] = make_string(m_config.info.instructions);
	}

	http_response response = json_response(rpc_result(id, std::move(result)));
	response.set_header("Mcp-Session-Id", create_session());
	respond(std::move(response));
}

void mcp::endpoint::on_subscribe(const json::value& id, const json::object& params, const std::string& session_id,
	bool subscribe, const responder& respond)
{
	const json::value* uri = params.if_contains("uri");
	if (uri == nullptr || !uri->is_string()) {
		respond(json_response(rpc_error(id, invalid_params, "uri is required")));
		return;
	}
	const std::string text(uri->get_string().c_str(), uri->get_string().size());
	bool too_many = false;
	{
		std::lock_guard lock(m_mutex);
		const auto found = m_sessions.find(session_id);
		if (found != m_sessions.end()) {
			auto& subscriptions = found->second.subscriptions;
			if (!subscribe) {
				subscriptions.erase(text);
			} else if (subscriptions.size() >= m_config.max_subscriptions_per_session && subscriptions.count(text) == 0) {
				too_many = true;
			} else {
				subscriptions.insert(text);
			}
		}
	}
	if (too_many) {
		respond(json_response(rpc_error(id, invalid_params, "too many subscriptions")));
		return;
	}
	respond(json_response(rpc_result(id, json::object{})));
}

void mcp::endpoint::handle_get(const http_request& request, const responder& respond)
{
	const std::string* accept = request.header("accept");
	if (accept == nullptr || lower_copy(*accept).find("text/event-stream") == std::string::npos) {
		respond(error_response(406, invalid_request, "Accept must include text/event-stream"));
		return;
	}
	std::string session_id;
	std::shared_ptr<event_channel> previous;
	http_response session_error;
	if (!lookup_session(request, session_id, previous, session_error)) {
		respond(std::move(session_error));
		return;
	}

	auto channel = std::make_shared<event_channel>();
	// The first frame tells the client the stream is attached: whatever is notified after it is delivered.
	channel->push(": stream open\n\n");
	{
		std::lock_guard lock(m_mutex);
		const auto found = m_sessions.find(session_id);
		if (found == m_sessions.end()) {
			// Deleted between the lookup and now.
			respond(error_response(404, invalid_request, "unknown session, initialize again"));
			return;
		}
		previous = found->second.channel;
		found->second.channel = channel;
	}
	// One stream per session: a reconnecting client replaces the one that has gone away.
	if (previous) {
		previous->close();
	}

	http_response response;
	response.status = 200;
	response.set_header("Content-Type", "text/event-stream");
	response.set_header("Cache-Control", "no-cache");
	response.stream = std::move(channel);
	respond(std::move(response));
}

void mcp::endpoint::handle_delete(const http_request& request, const responder& respond)
{
	std::string session_id;
	std::shared_ptr<event_channel> channel;
	http_response session_error;
	if (!lookup_session(request, session_id, channel, session_error)) {
		respond(std::move(session_error));
		return;
	}
	{
		std::lock_guard lock(m_mutex);
		m_sessions.erase(session_id);
	}
	if (channel) {
		channel->close();
	}
	respond(http_response{});
}

void mcp::endpoint::notify_sessions(const std::string* uri, const std::string& frame)
{
	std::vector<std::shared_ptr<event_channel>> targets;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& entry : m_sessions) {
			const session& s = entry.second;
			if (!s.channel) {
				continue;
			}
			if (uri == nullptr || s.subscriptions.count(*uri) != 0) {
				targets.push_back(s.channel);
			}
		}
	}
	for (const std::shared_ptr<event_channel>& channel : targets) {
		channel->push(frame);
	}
}

void mcp::endpoint::notify_resource_updated(const std::string& uri)
{
	json::object params;
	params["uri"] = make_string(uri);
	notify_sessions(&uri, sse_message(json::serialize(notification("notifications/resources/updated", std::move(params)))));
}

void mcp::endpoint::notify_tools_changed()
{
	notify_sessions(nullptr, sse_message(json::serialize(notification("notifications/tools/list_changed", json::value(nullptr)))));
}

void mcp::endpoint::close_all()
{
	std::map<std::string, session> dropped;
	{
		std::lock_guard lock(m_mutex);
		dropped.swap(m_sessions);
	}
	for (auto& entry : dropped) {
		if (entry.second.channel) {
			entry.second.channel->close();
		}
	}
}
