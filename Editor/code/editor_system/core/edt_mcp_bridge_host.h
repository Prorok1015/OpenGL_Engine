#pragma once
#include <snk/mcp/mcp_host.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace edt
{
	class agent_access;
	class agent_policy;
	class editor_events;
	class job_service;

	// How the bridge follows the job a `starts_job` command's call started (a result `{job: N}`, N > 0) for a client that
	// asked for progress.
	struct mcp_bridge_options
	{
		// Both set: such a call is answered when its job ends, its progress forwarded meanwhile. Either null: the call
		// is answered at once with `{job}`. Both outlive the bridge.
		job_service* jobs = nullptr;
		editor_events* events = nullptr;
		// Empty: std::chrono::steady_clock (tests pass their own).
		std::function<std::chrono::steady_clock::time_point()> clock;
		// At most one notifications/progress per call in this time; the latest value waits for the next slot.
		std::chrono::milliseconds progress_interval{ 250 };
		// After this the call is answered with the job still running, and its progress is no longer forwarded.
		std::chrono::milliseconds hold_limit{ std::chrono::minutes(10) };
	};

	// Whether an agent sees a command as a tool (AI_INTEGRATION.md section 5, A6b). Decided by the policy alone:
	//  - the command is offered as a tool at all (`expose_as_tool`);
	//  - its category is one an agent can have (not `unset`, not `not_for_agents`): whatever the policy files say, a
	//    command nobody classified does not exist for an agent;
	//  - the effective mode (project layer or the owner's defaults, the person's layer, the live switch) is not `deny`.
	// An `ask` command is listed: calling it asks a person. Main thread only (the policy's rule).
	bool agent_may_use(const cmd::command_desc& command, agent_policy& policy);

	// "viewport.capture" -> "viewport_capture"; "x.y" at version 2 -> "x_y_v2".
	std::string agent_tool_name(const cmd::command_desc& command);

	// The editor's side of the MCP server: the module (snk_mcp_server) knows nothing of the engine, this class turns
	// its mcp::host calls into calls through agent_access (the policy and the confirmations) and the command registry.
	//
	// Threads. The module calls the host from its I/O thread. The registry belongs to the main thread, so every
	// host call hops there first (ds::task_system::post_main, drained by the `tasks` frame stage) and a command runs
	// through agent_confirmations::invoke as origin `agent` with the MCP session id. The I/O thread never touches
	// the registry and never waits for the main thread: the answer (`done`) is called from the main thread, possibly
	// frames later (a person is asked, a gesture is waited out), and the module posts it back to its own thread.
	// What a `done` closure holds is only that answer, never the bridge or a connection.
	//
	// A call whose client sent a progressToken (request_context::wants_progress) to a command_desc::starts_job command
	// that answered with a running job (`{job: N}`, N > 0) is held, at most 16 per session (more are answered at once):
	// the job's progress goes out as notifications/progress (fraction of 1, strictly increasing, the job's note) and the
	// answer comes when the job ends, the result object plus `status` (job_status_name), `result` (the job's JSON; left
	// out with `result_dropped` past half the response limit) and `error` (failed). It is released earlier with
	// `status: "running"` after mcp_bridge_options::hold_limit (tick()). notifications/cancelled for it, or the end of its
	// MCP session, also while it still waits for a person, cancels the job and drops `done` uncalled (the transport
	// completes the request without an answer).
	//
	// Create and destroy on the main thread. Destroy it after the server that uses it (it has stopped by then) and
	// before the agent_access it points to: calls still waiting in the main queue find the host gone and are answered
	// with an error or dropped.
	class mcp_bridge_host final : public mcp::host
	{
	public:
		// `on_session_closed` is called on the main thread when an MCP session ended (disconnect, idle timeout, an
		// explicit close); the editor turns it into editor_events::agent_session_closed.
		using session_closed_hook = std::function<void(const std::string& session_id)>;

		mcp_bridge_host(cmd::command_registry& commands, ds::task_system& tasks, agent_access& access, session_closed_hook on_session_closed = {},
			mcp_bridge_options options = {});
		~mcp_bridge_host() override;
		mcp_bridge_host(const mcp_bridge_host&) = delete;
		mcp_bridge_host& operator=(const mcp_bridge_host&) = delete;

		void list_tools(std::function<void(std::vector<mcp::tool_info>)> done) override;
		void call_tool(mcp::request_context context, std::string name, boost::json::value args, mcp::progress_sink progress,
			std::function<void(mcp::call_result)> done) override;
		void list_resources(std::function<void(std::vector<mcp::resource_info>)> done) override;
		void read_resource(mcp::request_context context, std::string uri, std::function<void(mcp::resource_result)> done) override;
		void log(mcp::log_level level, std::string_view message) override;
		void session_closed(std::string session_id) override;
		void cancel_request(std::string session_id, boost::json::value request_id) override;

		// Once per frame, main thread: sends the progress that waited for its slot and releases the calls held past
		// hold_limit.
		void tick();

		// The server is stopping with the editor: its sessions ending now are not reported (the subscribers are going too).
		void stop_reporting();

	private:
		struct state;

		ds::task_system& m_tasks;
		std::shared_ptr<state> m_state;
	};
}
