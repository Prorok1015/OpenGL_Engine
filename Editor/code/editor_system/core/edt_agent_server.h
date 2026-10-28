#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace cmd { class command_registry; }
namespace ds { class task_system; }

namespace edt
{
	// The folder whose .snk/ receives mcp.json: `configured` (editor/project_dir) when set, else the open project's
	// root, else the working directory.
	std::filesystem::path agent_project_dir(const std::filesystem::path& configured,
		const std::optional<std::filesystem::path>& project_root);

	struct agent_server_options
	{
		// 0: the operating system picks a free port.
		std::uint16_t port = 0;
		// <project_dir>/.snk/mcp.json receives the port and the token for the client.
		std::filesystem::path project_dir;
	};

	// The MCP server of the editor (module snk_mcp_server) with everything it needs on the editor's side: the
	// main-thread queue, the bridge host and the connection file. Off by default (editor/agent_server). Read-only in
	// this step: tools are the non-mutating commands of the registry, resources are the log, the jobs, the dialogs and
	// the screenshots of the two views.
	//
	// Create and destroy on the main thread, before the command registry and the task system go. The destructor stops
	// the server, so no thread of the module is running after it, and removes the connection file.
	class agent_server
	{
	public:
		agent_server(cmd::command_registry& commands, ds::task_system& tasks, agent_server_options options);
		~agent_server();
		agent_server(const agent_server&) = delete;
		agent_server& operator=(const agent_server&) = delete;

		bool running() const;
		// The bound port; 0 when the server did not start.
		std::uint16_t port() const;

	private:
		struct impl;
		std::unique_ptr<impl> m_impl;
	};
}
