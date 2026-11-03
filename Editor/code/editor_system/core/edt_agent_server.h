#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmd { class command_registry; }
namespace edt { class agent_access; }
namespace ds { class task_system; }

namespace edt
{
	// The folder whose .snk/ receives mcp.json: `configured` (editor/project_dir) when set, else the open project's
	// root, else the working directory.
	std::filesystem::path agent_project_dir(const std::filesystem::path& configured,
		const std::optional<std::filesystem::path>& project_root);

	// One MCP session of the HTTP transport as the agents window shows it. `client_name` is what the client called itself
	// (untrusted text, already cut and cleaned by the endpoint).
	struct agent_session_info
	{
		std::string id;
		std::string client_name;
		std::chrono::seconds age{};
		std::chrono::seconds idle{};
		bool listening = false;
	};

	struct agent_server_options
	{
		// MCP over Streamable HTTP on 127.0.0.1 (editor/agent_server), with the connection file.
		bool http = true;
		// 0: the operating system picks a free port.
		std::uint16_t port = 0;
		// <project_dir>/.snk/mcp.json receives the port and the token for the client.
		std::filesystem::path project_dir;
		// An MCP session with no request for this long is closed. 0: never.
		std::chrono::minutes session_idle_timeout{ 30 };
		// MCP over stdin/stdout (--mcp-stdio): writes one message line (the saved stdout, platform::protocol_stdout);
		// false when the client is gone. Called on the transport's writer thread. Null: no stdio transport.
		std::function<bool(std::string_view)> stdio_write;
		// The stdio session ended (stdin closed, or the client stopped taking the output). Called on a transport thread.
		std::function<void()> on_stdio_eof;
		// Called on the main thread when an MCP session ended, except while the server itself stops. Empty: nothing.
		std::function<void(const std::string& session_id)> on_session_closed;
	};

	// The MCP server of the editor (module snk_mcp_server) with everything it needs on the editor's side: the
	// bridge host, shared by its two transports: Streamable HTTP with the connection file
	// (editor/agent_server) and stdio (editor/mcp_stdio, #452). Both are off by default. Tools are the commands of the
	// registry the bridge lets agents use, resources are the log, the jobs, the dialogs and the screenshots of the two views.
	//
	// Create and destroy on the main thread, before the command registry and the task system go. The destructor stops
	// both transports, so no thread of the module is running after it, and removes the connection file.
	class agent_server
	{
	public:
		// `access` outlives the server (destroy the server first).
		agent_server(cmd::command_registry& commands, ds::task_system& tasks, agent_access& access, agent_server_options options);
		~agent_server();
		agent_server(const agent_server&) = delete;
		agent_server& operator=(const agent_server&) = delete;

		// The HTTP transport, on and off while the editor runs (#449). start_http makes a new token and a new connection
		// file; false (and nothing runs) when the port cannot be bound or the file cannot be written; true when it already
		// runs on that port. A different port is a restart. stop_http ends every session through the usual
		// `on_session_closed` (unlike the destructor, which is the editor going down) and removes the connection file.
		// The stdio transport is not touched.
		bool start_http(std::uint16_t port);
		void stop_http();
		// The open sessions of the HTTP transport, oldest first, and the end of one of them (reported like any end).
		std::vector<agent_session_info> sessions() const;
		bool close_session(const std::string& id);

		// The HTTP server runs.
		bool running() const;
		// The bound port; 0 when the HTTP server did not start.
		std::uint16_t port() const;
		// The MCP session of the stdio client; empty without the transport or before its initialize.
		std::string stdio_session() const;
		// Tells the connected clients of both transports to list the tools again (a command's `enabled` gate changed,
		// e.g. live mode).
		void notify_tools_changed();

	private:
		struct impl;

		void launch_http();

		std::unique_ptr<impl> m_impl;
	};
}
