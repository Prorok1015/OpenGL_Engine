#pragma once
#include <snk/editor/edt_mcp_bridge_host.h>
#include <mcp/mcp_server.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace edt
{
	// The editor's MCP server (AI_INTEGRATION A6a): the module's server, the bridge to the command registry and the
	// queue that brings calls to the main thread. Off unless `editor/agent_server` is set; the user turns it on.
	// Create, pump and destroy on the main thread.
	class mcp_service
	{
	public:
		struct settings
		{
			// 0: the system picks a free port.
			std::uint16_t port = 0;
			// Where the port and the token go for clients: <project>/.snk/mcp.json. Readable by the user only.
			std::filesystem::path connection_file;
			std::function<std::vector<std::string>()> viewport_ids;
		};

		mcp_service(cmd::command_registry& registry, ds::task_system& tasks, settings opts);
		// Stops the server first, then the bridge, then the queue.
		~mcp_service();
		mcp_service(const mcp_service&) = delete;
		mcp_service& operator=(const mcp_service&) = delete;

		[[nodiscard]] bool start();
		bool running() const { return m_server.running(); }
		std::uint16_t port() const { return m_server.port(); }

		// Once per frame: follows changes of the command set and of the log, tells subscribed clients.
		void pump();

		mcp_bridge_host& bridge() { return m_bridge; }

	private:
		// Declaration order is the reverse of destruction: the server stops before the bridge and the queue go.
		cmd::main_thread_queue m_queue;
		mcp_bridge_host m_bridge;
		mcp::server m_server;
		std::uint64_t m_log_seq = 0;
		int m_frames_until_log_check = 0;
	};
}
