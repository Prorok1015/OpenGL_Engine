#pragma once
#include "mcp_config.h"
#include "mcp_host.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The MCP protocol over HTTP without a socket: requests come in as plain data, answers go out through callbacks.
// `server` puts it on a loopback TCP port; the tests drive it directly with a fake host.
namespace mcp
{
	struct http_request
	{
		std::string method;
		// Path and query as sent ("/mcp").
		std::string target;
		// As received; names are matched case-insensitively.
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;

		std::optional<std::string> header(std::string_view name) const;
	};

	struct http_response
	{
		int status = 200;
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;
		// An SSE stream: the transport sends the head, keeps the connection open and delivers what the endpoint
		// writes to the stream_sink of the request. `body` is empty.
		bool stream = false;
		// The transport must close the connection after this response.
		bool close = false;

		void set(std::string name, std::string value) { headers.emplace_back(std::move(name), std::move(value)); }
	};

	// Where the endpoint writes server-sent events of one HTTP exchange. Called from any thread, must not block.
	class stream_sink
	{
	public:
		virtual ~stream_sink() = default;
		// Bytes of the SSE body, ready to send.
		virtual void write(std::string_view chunk) = 0;
		// No more events: end the response and close the connection.
		virtual void close() = 0;
	};

	using reply_fn = std::function<void(http_response)>;

	class endpoint
	{
	public:
		// `cfg.token` must not be empty; with an empty one every request is refused.
		endpoint(host& served, server_config cfg);
		~endpoint();
		endpoint(const endpoint&) = delete;
		endpoint& operator=(const endpoint&) = delete;

		// The port the transport listens on; the Host header must name it. 0: any port.
		void set_port(std::uint16_t port);

		// The checks that need only the request head: Host and Origin (DNS rebinding), the bearer token, path,
		// method, content type and size. A transport calls it before it reads the body, so a refused client never
		// gets its message read. Returns the refusal (it asks to close the connection), or nothing.
		std::optional<http_response> screen(const http_request& head, std::uint64_t content_length) const;

		// Handles a complete request (screening it again) and calls `reply` exactly once, on any thread, maybe
		// before returning. `sink` carries the events of a streamed answer; null: answers are never streamed.
		void handle(http_request request, reply_fn reply, std::shared_ptr<stream_sink> sink);

		// To the sessions that subscribed to `uri` and have an open event stream.
		void notify_resource_updated(const std::string& uri);
		// To every session with an open event stream.
		void notify_tools_changed();
		// Ends every session and closes their streams.
		void close_all();

		std::size_t session_count() const;

	private:
		struct impl;
		std::shared_ptr<impl> m_impl;
	};
}
