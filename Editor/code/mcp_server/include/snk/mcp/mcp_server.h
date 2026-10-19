#pragma once
#include <snk/mcp/mcp_endpoint.h>
#include <snk/mcp/mcp_host.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mcp
{
	namespace detail { class server_core; }

	struct server_config
	{
		// 0: the operating system picks a free port (read it back with port()).
		std::uint16_t port = 0;
		// Bearer token (generate_token()). Empty: start() fails.
		std::string token;
		// Origin header values that are accepted; see endpoint_config.
		std::vector<std::string> allowed_origins;
		std::size_t max_connections = 16;
		std::size_t max_sessions = 8;
		std::size_t max_header_bytes = 16 * 1024;
		std::size_t max_message_bytes = 1024 * 1024;
		// A request must arrive within this time of the connection opening or of the previous response.
		std::chrono::milliseconds read_timeout{ 60000 };
		server_info info;
	};

	// The MCP server: Streamable HTTP on 127.0.0.1 over one I/O thread of its own. It listens on the loopback
	// interface only, and a request without the token is answered with 401 and the connection is closed.
	// The host must outlive the server. start() once; the server cannot be restarted.
	class server
	{
	public:
		server(host& application, server_config config);
		// stop()
		~server();
		server(const server&) = delete;
		server& operator=(const server&) = delete;

		// Binds the port and starts the I/O thread. False, and the reason goes to host::log, when the port cannot be
		// bound or the token is empty.
		bool start();
		// Closes the connections and joins the I/O thread. Safe to call twice.
		void stop();

		bool running() const;
		// The bound port; 0 before start().
		std::uint16_t port() const;

		// Thread-safe.
		void notify_resource_updated(const std::string& uri);
		void notify_tools_changed();

	private:
		std::shared_ptr<detail::server_core> m_core;
	};
}
