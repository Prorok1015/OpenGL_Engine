#include <mcp/mcp_endpoint.h>
#include "mcp_util.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <atomic>
#include <exception>
#include <map>
#include <mutex>
#include <set>

namespace json = boost::json;

namespace {
	using namespace mcp::util;

	constexpr std::string_view latest_protocol = "2025-06-18";
	// Streamable HTTP (2025-03-26 and later). 2024-11-05 is the old two-endpoint HTTP+SSE transport.
	bool supported_protocol(std::string_view version)
	{
		return version == "2025-06-18" || version == "2025-03-26";
	}

	// JSON-RPC error codes.
	constexpr int parse_error = -32700;
	constexpr int invalid_request = -32600;
	constexpr int method_not_found = -32601;
	constexpr int invalid_params = -32602;
	constexpr int server_busy = -32000;
	constexpr int resource_not_found = -32002;

	constexpr std::size_t max_subscriptions = 64;

	json::value id_copy(const json::value* id)
	{
		return id ? *id : json::value(nullptr);
	}

	json::object rpc_error(const json::value* id, int code, std::string_view message)
	{
		json::object error;
		error["code"] = code;
		error["message"] = js(message);
		json::object out;
		out["jsonrpc"] = "2.0";
		out["id"] = id_copy(id);
		out["error"] = std::move(error);
		return out;
	}

	json::object rpc_result(const json::value& id, json::value result)
	{
		json::object out;
		out["jsonrpc"] = "2.0";
		out["id"] = id;
		out["result"] = std::move(result);
		return out;
	}

	json::object rpc_notification(std::string_view method, json::value params)
	{
		json::object out;
		out["jsonrpc"] = "2.0";
		out["method"] = js(method);
		out["params"] = std::move(params);
		return out;
	}

	mcp::http_response json_response(int status, const json::value& body)
	{
		mcp::http_response out;
		out.status = status;
		out.set("Content-Type", "application/json");
		out.body = json::serialize(body);
		return out;
	}

	// A refusal before any JSON-RPC message was read.
	mcp::http_response refusal(int status, std::string_view message)
	{
		json::object body;
		body["error"] = js(message);
		mcp::http_response out = json_response(status, body);
		out.close = true;
		return out;
	}

	mcp::http_response accepted()
	{
		mcp::http_response out;
		out.status = 202;
		return out;
	}

	mcp::http_response event_stream_head()
	{
		mcp::http_response out;
		out.status = 200;
		out.set("Content-Type", "text/event-stream");
		out.set("Cache-Control", "no-cache");
		out.stream = true;
		out.close = true;
		return out;
	}

	std::string sse_event(const json::value& message)
	{
		return "event: message\ndata: " + json::serialize(message) + "\n\n";
	}

	json::object text_item(std::string_view text)
	{
		json::object item;
		item["type"] = "text";
		item["text"] = js(text);
		return item;
	}

	json::object tool_result_json(const mcp::call_result& result)
	{
		json::array content;
		for (const mcp::content& item : result.items) {
			if (item.type == mcp::content::kind::image) {
				json::object image;
				image["type"] = "image";
				image["data"] = js(item.data);
				image["mimeType"] = js(item.mime_type);
				content.push_back(std::move(image));
			} else {
				content.push_back(text_item(item.text));
			}
		}
		// Clients that ignore structuredContent still get the data.
		if (content.empty() && !result.structured.is_null()) content.push_back(text_item(json::serialize(result.structured)));
		if (content.empty() && result.is_error) content.push_back(text_item("the tool failed"));

		json::object out;
		out["content"] = std::move(content);
		out["isError"] = result.is_error;
		if (!result.is_error && result.structured.is_object()) out["structuredContent"] = result.structured;
		return out;
	}

	json::object resource_json(const mcp::resource_info& info)
	{
		json::object out;
		out["uri"] = js(info.uri);
		out["name"] = js(info.name.empty() ? info.uri : info.name);
		if (!info.description.empty()) out["description"] = js(info.description);
		if (!info.mime_type.empty()) out["mimeType"] = js(info.mime_type);
		return out;
	}

	json::object resource_content_json(const mcp::resource_content& content)
	{
		json::object out;
		out["uri"] = js(content.uri);
		if (!content.mime_type.empty()) out["mimeType"] = js(content.mime_type);
		if (content.is_blob) out["blob"] = js(content.blob);
		else out["text"] = js(content.text);
		return out;
	}

	const json::value* progress_token_of(const json::object& params)
	{
		const json::value* meta = params.if_contains("_meta");
		if (!meta || !meta->is_object()) return nullptr;
		const json::value* token = meta->as_object().if_contains("progressToken");
		if (!token || !(token->is_string() || token->is_int64() || token->is_uint64())) return nullptr;
		return token;
	}

	struct session
	{
		std::string id;
		std::uint64_t last_used = 0;
		std::set<std::string> subscriptions;
		// The GET stream for server-initiated messages.
		std::shared_ptr<mcp::stream_sink> stream;
	};
}

std::optional<std::string> mcp::http_request::header(std::string_view name) const
{
	for (const auto& [key, value] : headers) {
		if (iequals(key, name)) return value;
	}
	return std::nullopt;
}

struct mcp::endpoint::impl : std::enable_shared_from_this<impl>
{
	impl(host& served_, server_config cfg_)
		: served(served_)
		, cfg(std::move(cfg_))
	{
	}

	host& served;
	const server_config cfg;
	std::atomic<std::uint16_t> port{ 0 };
	std::atomic<std::size_t> in_flight{ 0 };

	mutable std::mutex mutex;
	std::map<std::string, session> sessions;
	std::uint64_t tick = 0;

	std::optional<http_response> screen(const http_request& head, std::uint64_t content_length) const;
	void handle(http_request request, reply_fn reply, std::shared_ptr<stream_sink> sink);

	void post(http_request request, reply_fn reply, std::shared_ptr<stream_sink> sink);
	void get(const http_request& request, reply_fn reply, std::shared_ptr<stream_sink> sink);
	void remove(const http_request& request, reply_fn reply);

	void initialize(const json::value& id, const json::value* params, reply_fn reply);
	void dispatch(const std::string& method, const json::value& id, const json::value* params, reply_fn reply, std::shared_ptr<stream_sink> sink);
	void tools_list(const json::value& id, reply_fn reply);
	void tools_call(const json::value& id, const json::value* params, reply_fn reply, std::shared_ptr<stream_sink> sink);
	void resources_list(const json::value& id, reply_fn reply);
	void resources_read(const json::value& id, const json::value* params, reply_fn reply);
	void resources_subscribe(const std::string& session_id, bool subscribe, const json::value& id, const json::value* params, reply_fn reply);

	bool touch(const std::string& session_id);
	bool acquire_slot();
	void release_slot() { in_flight.fetch_sub(1); }
	void broadcast(const std::string& event, const std::string* uri);
};

std::optional<mcp::http_response> mcp::endpoint::impl::screen(const http_request& head, std::uint64_t content_length) const
{
	// DNS rebinding: a page on another origin can make the browser send requests to 127.0.0.1 with its own Host.
	const std::optional<std::string> host_header = head.header("host");
	const authority where = host_header ? split_authority(*host_header) : authority{};
	const std::uint16_t bound = port.load();
	if (!where.valid || !is_loopback_host(where.host) || (bound != 0 && where.port && *where.port != bound) || (bound != 0 && !where.port)) {
		return refusal(403, "the Host header does not name this server");
	}
	if (const std::optional<std::string> origin = head.header("origin"); origin && !origin_allowed(*origin, cfg.allowed_origins)) {
		return refusal(403, "the Origin is not allowed");
	}
	if (!bearer_matches(head.header("authorization"), cfg.token)) {
		http_response out = refusal(401, "a valid bearer token is required");
		out.set("WWW-Authenticate", "Bearer realm=\"snk-mcp\"");
		return out;
	}

	const std::string_view target = head.target;
	if (target.substr(0, target.find('?')) != cfg.path) return refusal(404, "no such endpoint");
	if (head.method != "POST" && head.method != "GET" && head.method != "DELETE") {
		http_response out = refusal(405, "use POST, GET or DELETE");
		out.set("Allow", "GET, POST, DELETE");
		return out;
	}
	if (content_length > cfg.max_message_bytes) return refusal(413, "the message is too large");
	if (head.method == "POST") {
		const std::optional<std::string> type = head.header("content-type");
		if (!type || !starts_with_nocase(trim(*type), "application/json")) return refusal(415, "the content type must be application/json");
	}
	return std::nullopt;
}

void mcp::endpoint::impl::handle(http_request request, reply_fn reply, std::shared_ptr<stream_sink> sink)
{
	if (auto refused = screen(request, request.body.size())) {
		reply(std::move(*refused));
		return;
	}
	if (request.method == "POST") post(std::move(request), std::move(reply), std::move(sink));
	else if (request.method == "GET") get(request, std::move(reply), std::move(sink));
	else remove(request, std::move(reply));
}

bool mcp::endpoint::impl::touch(const std::string& session_id)
{
	std::lock_guard lock(mutex);
	const auto found = sessions.find(session_id);
	if (found == sessions.end()) return false;
	found->second.last_used = ++tick;
	return true;
}

bool mcp::endpoint::impl::acquire_slot()
{
	std::size_t current = in_flight.load();
	while (current < cfg.max_in_flight) {
		if (in_flight.compare_exchange_weak(current, current + 1)) return true;
	}
	return false;
}

void mcp::endpoint::impl::broadcast(const std::string& event, const std::string* uri)
{
	std::vector<std::shared_ptr<stream_sink>> targets;
	{
		std::lock_guard lock(mutex);
		for (const auto& [id, s] : sessions) {
			if (!s.stream) continue;
			if (uri && !s.subscriptions.contains(*uri)) continue;
			targets.push_back(s.stream);
		}
	}
	for (const auto& target : targets) target->write(event);
}

void mcp::endpoint::impl::post(http_request request, reply_fn reply, std::shared_ptr<stream_sink> sink)
{
	if (!accepts(request.header("accept"), "application/json")) {
		reply(refusal(406, "the client must accept application/json"));
		return;
	}

	json::error_code ec;
	json::parse_options options;
	options.max_depth = 32;
	const json::value document = json::parse(request.body, ec, json::storage_ptr(), options);
	if (ec) {
		reply(json_response(400, rpc_error(nullptr, parse_error, "the body is not valid JSON")));
		return;
	}
	if (!document.is_object()) {
		reply(json_response(400, rpc_error(nullptr, invalid_request, "send one JSON-RPC message object per request (batches are not supported)")));
		return;
	}

	const json::object& message = document.as_object();
	const json::value* method_value = message.if_contains("method");
	if (!method_value) {
		// A response to a request of ours: the server sends none.
		if (message.contains("result") || message.contains("error")) reply(accepted());
		else reply(json_response(400, rpc_error(nullptr, invalid_request, "not a JSON-RPC request or notification")));
		return;
	}
	if (!method_value->is_string()) {
		reply(json_response(400, rpc_error(nullptr, invalid_request, "method must be a string")));
		return;
	}
	const std::string method(method_value->as_string());
	const json::value* id = message.if_contains("id");
	if (id && !(id->is_string() || id->is_int64() || id->is_uint64())) {
		reply(json_response(400, rpc_error(nullptr, invalid_request, "id must be a string or an integer")));
		return;
	}
	const json::value* params = message.if_contains("params");

	if (id && method == "initialize") {
		initialize(*id, params, std::move(reply));
		return;
	}

	const std::optional<std::string> session_id = request.header("mcp-session-id");
	if (!session_id || session_id->empty()) {
		reply(json_response(400, rpc_error(id, invalid_request, "the Mcp-Session-Id header is required: call initialize first")));
		return;
	}
	if (!touch(*session_id)) {
		reply(json_response(404, rpc_error(id, invalid_request, "unknown session: call initialize again")));
		return;
	}
	if (const std::optional<std::string> version = request.header("mcp-protocol-version"); version && !supported_protocol(*version)) {
		reply(json_response(400, rpc_error(id, invalid_request, "unsupported MCP-Protocol-Version")));
		return;
	}

	if (!id) {
		// Notifications (initialized, cancelled, ...) need no answer.
		reply(accepted());
		return;
	}
	if (method == "resources/subscribe" || method == "resources/unsubscribe") {
		resources_subscribe(*session_id, method == "resources/subscribe", *id, params, std::move(reply));
		return;
	}
	dispatch(method, *id, params, std::move(reply), std::move(sink));
}

void mcp::endpoint::impl::initialize(const json::value& id, const json::value* params, reply_fn reply)
{
	if (!params || !params->is_object()) {
		reply(json_response(200, rpc_error(&id, invalid_params, "initialize needs params")));
		return;
	}
	std::string version(latest_protocol);
	if (const json::value* requested = params->as_object().if_contains("protocolVersion"); requested && requested->is_string()) {
		const std::string asked(requested->as_string());
		if (supported_protocol(asked)) version = asked;
	}

	std::shared_ptr<stream_sink> evicted;
	std::string session_id = random_hex(16);
	{
		std::lock_guard lock(mutex);
		if (!sessions.empty() && sessions.size() >= cfg.max_sessions) {
			// A client that died without DELETE must not lock the others out: the least recently used one goes.
			auto oldest = sessions.begin();
			for (auto it = sessions.begin(); it != sessions.end(); ++it) {
				if (it->second.last_used < oldest->second.last_used) oldest = it;
			}
			evicted = std::move(oldest->second.stream);
			sessions.erase(oldest);
		}
		session fresh;
		fresh.id = session_id;
		fresh.last_used = ++tick;
		sessions.emplace(session_id, std::move(fresh));
	}
	if (evicted) evicted->close();

	json::object tools_capability;
	tools_capability["listChanged"] = true;
	json::object resources_capability;
	resources_capability["subscribe"] = true;
	resources_capability["listChanged"] = false;
	json::object capabilities;
	capabilities["tools"] = std::move(tools_capability);
	capabilities["resources"] = std::move(resources_capability);

	json::object info;
	info["name"] = js(cfg.server_name);
	info["version"] = js(cfg.server_version);
	if (!cfg.api_version.empty()) info["snk_api"] = js(cfg.api_version);

	json::object result;
	result["protocolVersion"] = js(version);
	result["capabilities"] = std::move(capabilities);
	result["serverInfo"] = std::move(info);
	if (!cfg.instructions.empty()) result["instructions"] = js(cfg.instructions);

	http_response out = json_response(200, rpc_result(id, std::move(result)));
	out.set("Mcp-Session-Id", session_id);
	reply(std::move(out));
}

void mcp::endpoint::impl::dispatch(const std::string& method, const json::value& id, const json::value* params, reply_fn reply, std::shared_ptr<stream_sink> sink)
{
	if (method == "ping") reply(json_response(200, rpc_result(id, json::object{})));
	else if (method == "tools/list") tools_list(id, std::move(reply));
	else if (method == "tools/call") tools_call(id, params, std::move(reply), std::move(sink));
	else if (method == "resources/list") resources_list(id, std::move(reply));
	else if (method == "resources/templates/list") reply(json_response(200, rpc_result(id, json::object{ { "resourceTemplates", json::array{} } })));
	else if (method == "resources/read") resources_read(id, params, std::move(reply));
	else reply(json_response(200, rpc_error(&id, method_not_found, "method not found: " + method)));
}

void mcp::endpoint::impl::tools_list(const json::value& id, reply_fn reply)
{
	json::array tools;
	for (const tool_info& tool : served.list_tools()) {
		json::object entry;
		entry["name"] = js(tool.name);
		entry["description"] = js(tool.description);
		entry["inputSchema"] = tool.input_schema.is_object() ? tool.input_schema : json::value(json::object{ { "type", "object" } });
		if (tool.output_schema.is_object()) entry["outputSchema"] = tool.output_schema;
		json::object annotations;
		annotations["readOnlyHint"] = tool.read_only;
		annotations["destructiveHint"] = tool.destructive;
		entry["annotations"] = std::move(annotations);
		tools.push_back(std::move(entry));
	}
	reply(json_response(200, rpc_result(id, json::object{ { "tools", std::move(tools) } })));
}

void mcp::endpoint::impl::tools_call(const json::value& id, const json::value* params, reply_fn reply, std::shared_ptr<stream_sink> sink)
{
	if (!params || !params->is_object()) {
		reply(json_response(200, rpc_error(&id, invalid_params, "tools/call needs params")));
		return;
	}
	const json::object& call = params->as_object();
	const json::value* name_value = call.if_contains("name");
	if (!name_value || !name_value->is_string()) {
		reply(json_response(200, rpc_error(&id, invalid_params, "tools/call needs a tool name")));
		return;
	}
	std::string name(name_value->as_string());
	json::value arguments = json::object{};
	if (const json::value* given = call.if_contains("arguments")) {
		if (!given->is_object()) {
			reply(json_response(200, rpc_error(&id, invalid_params, "arguments must be an object")));
			return;
		}
		arguments = *given;
	}

	bool known = false;
	for (const tool_info& tool : served.list_tools()) {
		if (tool.name == name) {
			known = true;
			break;
		}
	}
	if (!known) {
		reply(json_response(200, rpc_error(&id, invalid_params, "unknown tool: " + name)));
		return;
	}
	if (!acquire_slot()) {
		reply(json_response(200, rpc_error(&id, server_busy, "too many calls are running, try again")));
		return;
	}

	// A call that asks for progress gets an event stream: the notifications, then the response.
	const json::value* token = progress_token_of(call);
	const bool streamed = token != nullptr && sink != nullptr;
	if (streamed) reply(event_stream_head());

	const auto finished = std::make_shared<std::atomic<bool>>(false);
	std::function<void(call_result)> done = [weak = weak_from_this(), id = id, reply, sink, streamed, finished](call_result result) {
		if (finished->exchange(true)) return;
		const auto self = weak.lock();
		if (!self) return;
		self->release_slot();
		json::object response = rpc_result(id, tool_result_json(result));
		if (streamed) {
			sink->write(sse_event(response));
			sink->close();
		} else {
			reply(json_response(200, response));
		}
	};

	progress_sink progress = [](double, std::optional<double>, std::string) {};
	if (streamed) {
		progress = [finished, sink, token = *token](double done_so_far, std::optional<double> total, std::string message) {
			if (finished->load()) return;
			json::object notification_params;
			notification_params["progressToken"] = token;
			notification_params["progress"] = done_so_far;
			if (total) notification_params["total"] = *total;
			if (!message.empty()) notification_params["message"] = js(message);
			sink->write(sse_event(rpc_notification("notifications/progress", std::move(notification_params))));
		};
	}

	try {
		served.call_tool(std::move(name), std::move(arguments), std::move(progress), done);
	} catch (const std::exception& e) {
		call_result failed;
		failed.is_error = true;
		failed.items.push_back(content::make_text(std::string("the tool threw: ") + e.what()));
		done(std::move(failed));
	}
}

void mcp::endpoint::impl::resources_list(const json::value& id, reply_fn reply)
{
	json::array resources;
	for (const resource_info& info : served.list_resources()) resources.push_back(resource_json(info));
	reply(json_response(200, rpc_result(id, json::object{ { "resources", std::move(resources) } })));
}

void mcp::endpoint::impl::resources_read(const json::value& id, const json::value* params, reply_fn reply)
{
	const json::value* uri_value = params && params->is_object() ? params->as_object().if_contains("uri") : nullptr;
	if (!uri_value || !uri_value->is_string()) {
		reply(json_response(200, rpc_error(&id, invalid_params, "resources/read needs a uri")));
		return;
	}
	std::string uri(uri_value->as_string());
	if (!acquire_slot()) {
		reply(json_response(200, rpc_error(&id, server_busy, "too many calls are running, try again")));
		return;
	}

	const auto finished = std::make_shared<std::atomic<bool>>(false);
	std::function<void(resource_result)> done = [weak = weak_from_this(), id = id, reply, uri, finished](resource_result result) {
		if (finished->exchange(true)) return;
		const auto self = weak.lock();
		if (!self) return;
		self->release_slot();
		if (!result.found) {
			const std::string reason = result.error.empty() ? "resource not found: " + uri : result.error;
			reply(json_response(200, rpc_error(&id, resource_not_found, reason)));
			return;
		}
		json::array contents;
		for (const resource_content& item : result.contents) contents.push_back(resource_content_json(item));
		reply(json_response(200, rpc_result(id, json::object{ { "contents", std::move(contents) } })));
	};

	try {
		served.read_resource(std::move(uri), done);
	} catch (const std::exception& e) {
		resource_result failed;
		failed.error = std::string("reading the resource failed: ") + e.what();
		done(std::move(failed));
	}
}

void mcp::endpoint::impl::resources_subscribe(const std::string& session_id, bool subscribe, const json::value& id, const json::value* params, reply_fn reply)
{
	const json::value* uri_value = params && params->is_object() ? params->as_object().if_contains("uri") : nullptr;
	if (!uri_value || !uri_value->is_string()) {
		reply(json_response(200, rpc_error(&id, invalid_params, "a uri is required")));
		return;
	}
	const std::string uri(uri_value->as_string());
	if (subscribe) {
		bool listed = false;
		for (const resource_info& info : served.list_resources()) {
			if (info.uri == uri) {
				listed = true;
				break;
			}
		}
		if (!listed) {
			reply(json_response(200, rpc_error(&id, resource_not_found, "resource not found: " + uri)));
			return;
		}
	}
	bool too_many = false;
	{
		std::lock_guard lock(mutex);
		const auto found = sessions.find(session_id);
		if (found != sessions.end()) {
			if (!subscribe) found->second.subscriptions.erase(uri);
			else if (found->second.subscriptions.size() >= max_subscriptions && !found->second.subscriptions.contains(uri)) too_many = true;
			else found->second.subscriptions.insert(uri);
		}
	}
	if (too_many) reply(json_response(200, rpc_error(&id, server_busy, "too many subscriptions")));
	else reply(json_response(200, rpc_result(id, json::object{})));
}

void mcp::endpoint::impl::get(const http_request& request, reply_fn reply, std::shared_ptr<stream_sink> sink)
{
	if (!accepts(request.header("accept"), "text/event-stream")) {
		reply(refusal(406, "the client must accept text/event-stream"));
		return;
	}
	const std::optional<std::string> session_id = request.header("mcp-session-id");
	if (!session_id || session_id->empty()) {
		reply(refusal(400, "the Mcp-Session-Id header is required"));
		return;
	}
	if (!sink) {
		reply(refusal(405, "this transport cannot stream"));
		return;
	}

	std::shared_ptr<stream_sink> previous;
	bool known = false;
	{
		std::lock_guard lock(mutex);
		const auto found = sessions.find(*session_id);
		if (found != sessions.end()) {
			known = true;
			found->second.last_used = ++tick;
			previous = std::move(found->second.stream);
			found->second.stream = sink;
		}
	}
	if (!known) {
		reply(refusal(404, "unknown session"));
		return;
	}
	reply(event_stream_head());
	// A comment line: it makes the client's request return and tells it the stream is registered.
	sink->write(": stream open\n\n");
	if (previous) previous->close();
}

void mcp::endpoint::impl::remove(const http_request& request, reply_fn reply)
{
	const std::optional<std::string> session_id = request.header("mcp-session-id");
	if (!session_id || session_id->empty()) {
		reply(refusal(400, "the Mcp-Session-Id header is required"));
		return;
	}
	std::shared_ptr<stream_sink> stream;
	bool found = false;
	{
		std::lock_guard lock(mutex);
		const auto it = sessions.find(*session_id);
		if (it != sessions.end()) {
			found = true;
			stream = std::move(it->second.stream);
			sessions.erase(it);
		}
	}
	if (stream) stream->close();
	if (!found) {
		reply(refusal(404, "unknown session"));
		return;
	}
	http_response out;
	out.status = 200;
	reply(std::move(out));
}

mcp::endpoint::endpoint(host& served, server_config cfg)
	: m_impl(std::make_shared<impl>(served, std::move(cfg)))
{
}

mcp::endpoint::~endpoint() = default;

void mcp::endpoint::set_port(std::uint16_t port)
{
	m_impl->port.store(port);
}

std::optional<mcp::http_response> mcp::endpoint::screen(const http_request& head, std::uint64_t content_length) const
{
	return m_impl->screen(head, content_length);
}

void mcp::endpoint::handle(http_request request, reply_fn reply, std::shared_ptr<stream_sink> sink)
{
	m_impl->handle(std::move(request), std::move(reply), std::move(sink));
}

void mcp::endpoint::notify_resource_updated(const std::string& uri)
{
	json::object params;
	params["uri"] = js(uri);
	m_impl->broadcast(sse_event(rpc_notification("notifications/resources/updated", std::move(params))), &uri);
}

void mcp::endpoint::notify_tools_changed()
{
	m_impl->broadcast(sse_event(rpc_notification("notifications/tools/list_changed", json::object{})), nullptr);
}

void mcp::endpoint::close_all()
{
	std::vector<std::shared_ptr<stream_sink>> streams;
	{
		std::lock_guard lock(m_impl->mutex);
		for (auto& [id, s] : m_impl->sessions) {
			if (s.stream) streams.push_back(std::move(s.stream));
		}
		m_impl->sessions.clear();
	}
	for (const auto& stream : streams) stream->close();
}

std::size_t mcp::endpoint::session_count() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->sessions.size();
}
