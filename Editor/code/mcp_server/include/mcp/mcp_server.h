#pragma once
#include "mcp_config.h"
#include "mcp_host.h"
#include <cstdint>
#include <memory>
#include <string>

// An MCP server over Streamable HTTP on 127.0.0.1 (docs/roadmap/design/AI_INTEGRATION.md section 3.7): POST/GET/DELETE
// on one path, a bearer token, Host and Origin checks, SSE for notifications and progress, Mcp-Session-Id sessions.
// It runs one io thread of its own and calls the host from it. This module depends on Boost only.
namespace mcp
{
	class server
	{
	public:
		server(host& served, server_config cfg);
		// Stops the server.
		~server();
		server(const server&) = delete;
		server& operator=(const server&) = delete;

		// Binds 127.0.0.1, writes the connection file if configured, starts the io thread. False (see error())
		// when the port cannot be bound or the file cannot be written; nothing keeps running then. True if already running.
		[[nodiscard]] bool start();
		// Closes the listener and every connection, joins the io thread, removes the connection file. Idempotent.
		// Calls to the host that are still running may finish later; their answers are dropped.
		void stop();
		bool running() const;

		// Valid after a successful start().
		std::uint16_t port() const;
		std::string token() const;
		std::string url() const;
		std::string error() const;

		// Thread-safe. Ignored while stopped.
		void notify_resource_updated(const std::string& uri);
		void notify_tools_changed();

	private:
		struct impl;
		std::unique_ptr<impl> m_impl;
	};
}
