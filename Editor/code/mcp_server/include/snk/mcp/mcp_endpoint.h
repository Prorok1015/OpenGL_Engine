#pragma once
#include <snk/mcp/mcp_host.h>
#include <snk/mcp/mcp_http.h>

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mcp
{
	struct server_info
	{
		std::string name = "snk-editor";
		std::string version = "0";
		// Version of the command set (serverInfo.snk_api); empty: not reported.
		std::string snk_api;
		// Sent to the client as `instructions`; empty: not sent.
		std::string instructions;
	};

	struct endpoint_config
	{
		// Bearer token. Empty: every request is refused (fail closed).
		std::string token;
		// Path of the MCP endpoint.
		std::string path = "/mcp";
		// Origin header values that are accepted (exact match). A request with any other Origin is refused; one
		// without Origin (not a browser) is fine.
		std::vector<std::string> allowed_origins;
		std::size_t max_sessions = 8;
		std::size_t max_subscriptions_per_session = 64;
		server_info info;
	};

	// MCP over Streamable HTTP without the sockets: one HTTP request in, one response out. It checks the Host and
	// Origin headers (DNS rebinding) and the token, keeps the sessions (Mcp-Session-Id), speaks JSON-RPC and calls the
	// host. Thread-safe: the server calls it from its I/O thread, tests from anywhere. `respond` is called exactly
	// once, possibly later and from another thread (when the host answers later).
	//
	// Supported: initialize, ping, tools/list, tools/call (with notifications/progress on the event stream),
	// resources/list, resources/templates/list, resources/read, resources/subscribe, resources/unsubscribe,
	// notifications/resources/updated and notifications/tools/list_changed on the event stream (GET).
	// Batches are refused (removed from the protocol in 2025-06-18).
	class endpoint
	{
	public:
		using responder = std::function<void(http_response)>;

		endpoint(host& application, endpoint_config config);
		~endpoint();
		endpoint(const endpoint&) = delete;
		endpoint& operator=(const endpoint&) = delete;

		// The port the server listens on; a Host header must name it. 0: any port.
		void set_port(std::uint16_t port);

		void handle(const http_request& request, responder respond);

		// The Host, Origin and token checks alone: nullopt when the request may go on, otherwise the refusal (403 or
		// 401, closing the connection). The server asks before it sends "100 Continue" for a body it would refuse.
		std::optional<http_response> check_access(const http_request& request) const;

		// To the sessions that subscribed to `uri` and have an event stream open.
		void notify_resource_updated(const std::string& uri);
		// To every session that has an event stream open.
		void notify_tools_changed();
		// Drops the sessions and closes their streams.
		void close_all();

		std::size_t session_count() const;

	private:
		struct session
		{
			std::uint64_t serial = 0; // creation order, for eviction
			std::set<std::string> subscriptions;
			std::shared_ptr<event_channel> channel;
		};

		// What the sessions are, behind a shared_ptr: an answer of the host that comes after the endpoint is gone
		// (a subscribe waiting for the resource list) still finds valid state.
		struct shared_state
		{
			std::mutex mutex;
			std::map<std::string, session> sessions;
			std::uint64_t next_serial = 1;
		};

		void route(const http_request& request, const responder& respond);
		void handle_post(const http_request& request, const responder& respond);
		void handle_get(const http_request& request, const responder& respond);
		void handle_delete(const http_request& request, const responder& respond);
		void dispatch_rpc(const boost::json::object& message, const http_request& request, const responder& respond);
		void on_initialize(const boost::json::value& id, const boost::json::object& params, const responder& respond);
		void on_subscribe(const boost::json::value& id, const boost::json::object& params, const std::string& session_id,
			bool subscribe, const responder& respond);

		// The session named by the Mcp-Session-Id header. False, with the HTTP error in `error`, when the header is
		// missing or names no session.
		bool lookup_session(const http_request& request, std::string& session_id, std::shared_ptr<event_channel>& channel,
			http_response& error) const;
		std::string create_session();

		bool host_header_ok(const http_request& request) const;
		bool origin_ok(const http_request& request) const;
		bool token_ok(const http_request& request) const;

		// uri == nullptr: every session with a stream.
		void notify_sessions(const std::string* uri, const std::string& frame);

		host& m_host;
		endpoint_config m_config;
		std::atomic<std::uint16_t> m_port{ 0 };

		std::shared_ptr<shared_state> m_state;
	};
}
