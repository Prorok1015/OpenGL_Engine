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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	using mcp::rpc_error;
	using mcp::rpc_notification;
	using mcp::rpc_result;
	using mcp::rpc_codes::internal_error;
	using mcp::rpc_codes::invalid_params;
	using mcp::rpc_codes::invalid_request;
	using mcp::rpc_codes::parse_error;

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
}

mcp::endpoint::endpoint(host& application, endpoint_config config)
	: m_host(application)
	, m_config(std::move(config))
	, m_rpc(application, m_config.info)
	, m_state(std::make_shared<shared_state>())
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
	std::lock_guard lock(m_state->mutex);
	return m_state->sessions.size();
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

std::optional<mcp::http_response> mcp::endpoint::check_access(const http_request& request) const
{
	// DNS rebinding: a page on another origin can make the browser connect to 127.0.0.1 under its own host name.
	if (!host_header_ok(request)) {
		return error_response(403, invalid_request, "forbidden host", true);
	}
	if (!origin_ok(request)) {
		return error_response(403, invalid_request, "forbidden origin", true);
	}
	if (!token_ok(request)) {
		http_response response = error_response(401, invalid_request, "a bearer token is required", true);
		response.set_header("WWW-Authenticate", "Bearer realm=\"snk-editor\"");
		return response;
	}
	return std::nullopt;
}

void mcp::endpoint::route(const http_request& request, const responder& respond)
{
	if (std::optional<http_response> denied = check_access(request)) {
		respond(std::move(*denied));
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
	expire_idle_sessions();
	std::lock_guard lock(m_state->mutex);
	const auto found = m_state->sessions.find(*header);
	if (found == m_state->sessions.end()) {
		error = error_response(404, invalid_request, "unknown session, initialize again");
		return false;
	}
	found->second.last_used = now();
	session_id = found->first;
	channel = found->second.channel;
	return true;
}

std::string mcp::endpoint::create_session()
{
	expire_idle_sessions();
	std::string id = new_session_id();
	std::vector<std::string> evicted;
	{
		std::lock_guard lock(m_state->mutex);
		// A client that never sent DELETE must not lock the others out: the oldest session makes room.
		while (!m_state->sessions.empty() && m_state->sessions.size() >= m_config.max_sessions) {
			auto oldest = m_state->sessions.begin();
			for (auto it = m_state->sessions.begin(); it != m_state->sessions.end(); ++it) {
				if (it->second.serial < oldest->second.serial) {
					oldest = it;
				}
			}
			if (oldest->second.channel) {
				oldest->second.channel->close();
			}
			evicted.push_back(oldest->first);
			m_state->sessions.erase(oldest);
		}
		session created;
		created.serial = m_state->next_serial++;
		created.last_used = now();
		m_state->sessions.emplace(id, std::move(created));
	}
	for (std::string& gone : evicted) {
		m_host.session_closed(std::move(gone));
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
		if (!is_supported_protocol(*version)) {
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
	rpc_message rpc = classify_rpc(message);
	if (rpc.type == rpc_message::kind::response) {
		respond(accepted_response());
		return;
	}
	if (rpc.type == rpc_message::kind::invalid) {
		respond(error_response(400, invalid_request, rpc.error));
		return;
	}
	const bool is_request = rpc.type == rpc_message::kind::request;
	if (rpc.bad_params) {
		if (is_request) {
			respond(json_response(rpc_error(rpc.id, invalid_params, "params must be an object")));
		} else {
			respond(accepted_response());
		}
		return;
	}

	if (rpc.method == "initialize" && is_request) {
		on_initialize(rpc.id, rpc.params, respond);
		return;
	}

	std::string session_id;
	std::shared_ptr<event_channel> channel;
	http_response session_error;
	if (!lookup_session(request, session_id, channel, session_error)) {
		respond(std::move(session_error));
		return;
	}
	rpc_session session;
	session.id = session_id;
	if (!is_request) {
		// notifications/initialized, notifications/cancelled and the rest need no answer.
		respond(accepted_response());
		m_rpc.notification(rpc.method, rpc.params, session);
		return;
	}

	// The stream the session has when the notification is sent: one opened after the call, or a reconnect, gets it.
	session.notify = [weak = std::weak_ptr<shared_state>(m_state), session_id](const json::value& note) {
		const std::shared_ptr<shared_state> state = weak.lock();
		if (!state) {
			return;
		}
		std::shared_ptr<event_channel> stream;
		{
			std::lock_guard lock(state->mutex);
			const auto found = state->sessions.find(session_id);
			if (found != state->sessions.end()) {
				stream = found->second.channel;
			}
		}
		if (stream) {
			stream->push(sse_message(json::serialize(note)));
		}
	};
	// The answer may come after the endpoint is gone, so the state is captured.
	session.subscribe = [state = m_state, limit = m_config.max_subscriptions_per_session, session_id](const std::string& uri, bool subscribe) {
		std::lock_guard lock(state->mutex);
		const auto found = state->sessions.find(session_id);
		if (found == state->sessions.end()) {
			return true;
		}
		auto& subscriptions = found->second.subscriptions;
		if (!subscribe) {
			subscriptions.erase(uri);
			return true;
		}
		if (subscriptions.size() >= limit && subscriptions.count(uri) == 0) {
			return false;
		}
		subscriptions.insert(uri);
		return true;
	};
	m_rpc.dispatch(rpc.method, rpc.id, rpc.params, session, [respond](json::value answer) {
		respond(json_response(answer));
	});
}

void mcp::endpoint::on_initialize(const json::value& id, const json::object& params, const responder& respond)
{
	bool accepted = false;
	http_response response = json_response(m_rpc.initialize(id, params, accepted));
	if (accepted) {
		response.set_header("Mcp-Session-Id", create_session());
	}
	respond(std::move(response));
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
		std::lock_guard lock(m_state->mutex);
		const auto found = m_state->sessions.find(session_id);
		if (found == m_state->sessions.end()) {
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
	bool erased = false;
	{
		std::lock_guard lock(m_state->mutex);
		erased = m_state->sessions.erase(session_id) > 0;
	}
	if (channel) {
		channel->close();
	}
	if (erased) {
		m_host.session_closed(std::move(session_id));
	}
	respond(http_response{});
}

void mcp::endpoint::notify_sessions(const std::string* uri, const std::string& frame)
{
	std::vector<std::shared_ptr<event_channel>> targets;
	{
		std::lock_guard lock(m_state->mutex);
		for (const auto& entry : m_state->sessions) {
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
	notify_sessions(&uri, sse_message(json::serialize(rpc_notification("notifications/resources/updated", std::move(params)))));
}

void mcp::endpoint::notify_tools_changed()
{
	notify_sessions(nullptr, sse_message(json::serialize(rpc_notification("notifications/tools/list_changed", json::value(nullptr)))));
}

std::chrono::steady_clock::time_point mcp::endpoint::now() const
{
	return m_config.clock ? m_config.clock() : std::chrono::steady_clock::now();
}

void mcp::endpoint::expire_idle_sessions() const
{
	if (m_config.session_idle_timeout <= std::chrono::milliseconds::zero()) {
		return;
	}
	std::vector<std::string> expired;
	std::vector<std::shared_ptr<event_channel>> channels;
	{
		std::lock_guard lock(m_state->mutex);
		const auto current = now();
		for (auto it = m_state->sessions.begin(); it != m_state->sessions.end();) {
			// A client that listens on the event stream is there, whether or not it sends requests.
			const bool listening = it->second.channel && !it->second.channel->closed();
			if (listening || current - it->second.last_used < m_config.session_idle_timeout) {
				++it;
				continue;
			}
			expired.push_back(it->first);
			if (it->second.channel) {
				channels.push_back(it->second.channel);
			}
			it = m_state->sessions.erase(it);
		}
	}
	for (const auto& channel : channels) {
		channel->close();
	}
	for (std::string& id : expired) {
		m_host.session_closed(std::move(id));
	}
}

void mcp::endpoint::close_all()
{
	std::map<std::string, session> dropped;
	{
		std::lock_guard lock(m_state->mutex);
		dropped.swap(m_state->sessions);
	}
	for (auto& entry : dropped) {
		if (entry.second.channel) {
			entry.second.channel->close();
		}
		m_host.session_closed(entry.first);
	}
}
