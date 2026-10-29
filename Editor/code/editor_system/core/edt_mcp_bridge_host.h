#pragma once
#include <snk/mcp/mcp_host.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <boost/json/value.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace edt
{
	// What an agent may see and call through MCP in the read-only server (AI_INTEGRATION.md, decision 2, step A6a).
	// Driven by the command metadata, and fail closed:
	//  - listed for tools at all (`expose_as_tool`), not `mutates`, no `risk` (confirm / deny_by_default), not `long_running`;
	//  - AND either a domain of read commands (the part of the id before the first dot: log, job, dialog, viewport) or
	//    one of the listed read commands (scene.query, node.get, component.schema, asset.query, selection.get,
	//    problems.list, build.settings.get).
	//    Metadata is only as good as the one who wrote it (a command that forgot `mutates` must not become an agent
	//    tool), so a new domain or command is opt-in here.
	//  - OR, for a command marked `agent_when_enabled` (cmd_types.h): one of the listed ones (live.status,
	//    live.rebuild_now, live.reload_now) with `expose_as_tool`, no `risk`, not `long_running`, and only while its
	//    `enabled` returns true (live mode is on: the person's consent, #445). It may mutate.
	// Confirmations and the project policy come with A6b/A6c; #449 turns the `agent_when_enabled` commands into policy rows.
	bool agent_may_use(const cmd::command_desc& command);

	// "viewport.capture" -> "viewport_capture"; "x.y" at version 2 -> "x_y_v2".
	std::string agent_tool_name(const cmd::command_desc& command);

	// The editor's side of the MCP server: the module (snk_mcp_server) knows nothing of the engine, this class turns
	// its mcp::host calls into cmd::command_registry calls.
	//
	// Threads. The module calls the host from its I/O thread. The registry belongs to the main thread, so every
	// host call hops there first (ds::task_system::post_main, drained by the `tasks` frame stage) and a command runs
	// through cmd::main_thread_queue::call_now_or_post, as origin `agent` with the MCP session id. The I/O thread
	// never touches the registry and never waits for the main thread: the answer (`done`) is called from the main
	// thread and the module posts it back to its own thread.
	//
	// Create and destroy on the main thread. Destroy it after the server that uses it (it has stopped by then):
	// calls still waiting in the main queue find the host gone and are answered with an error or dropped.
	class mcp_bridge_host final : public mcp::host
	{
	public:
		mcp_bridge_host(cmd::command_registry& commands, cmd::main_thread_queue& queue, ds::task_system& tasks);
		~mcp_bridge_host() override;
		mcp_bridge_host(const mcp_bridge_host&) = delete;
		mcp_bridge_host& operator=(const mcp_bridge_host&) = delete;

		void list_tools(std::function<void(std::vector<mcp::tool_info>)> done) override;
		void call_tool(mcp::request_context context, std::string name, boost::json::value args, mcp::progress_sink progress,
			std::function<void(mcp::call_result)> done) override;
		void list_resources(std::function<void(std::vector<mcp::resource_info>)> done) override;
		void read_resource(mcp::request_context context, std::string uri, std::function<void(mcp::resource_result)> done) override;
		void log(mcp::log_level level, std::string_view message) override;

	private:
		struct state;

		ds::task_system& m_tasks;
		std::shared_ptr<state> m_state;
	};
}
