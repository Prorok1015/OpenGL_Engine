#pragma once
#include <mcp/mcp_host.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace edt
{
	// The editor's side of the MCP server (AI_INTEGRATION A6a): turns the commands of the process-wide
	// cmd::command_registry into MCP tools and resources.
	//
	// Read-only: only commands that are exposed (`expose_as_tool`) and do not change anything (`!mutates`, not
	// `long_running`, `risk == none`) become tools. The rule is the command's own metadata; there is no list here.
	// The mutating tools come with the undo-aware commands (A5, A6b).
	//
	// Threads. The registry belongs to the main thread, so the server's io thread never touches it: `refresh()` (main
	// thread, once per frame) copies the tool and resource lists into a snapshot that `list_tools()` and
	// `list_resources()` serve from any thread, and a call goes to the main thread through cmd::main_thread_queue, which
	// runs it when the `tasks` frame stage drains the main queue. One waiter thread watches the futures of the posted
	// calls and hands each result to the server; it never runs a command.
	class mcp_bridge_host final : public mcp::host
	{
	public:
		struct options
		{
			// Ids of the viewports `viewport.capture` can read ("scene", "game"). Called by refresh(), so on the main thread.
			std::function<std::vector<std::string>()> viewport_ids;
			// A screenshot larger than this (base64 characters) is refused: the client asks for a smaller size.
			std::size_t max_image_base64_bytes = 8 * 1024 * 1024;
			// How long a call may wait for the main thread to run it.
			std::chrono::milliseconds call_timeout{ 30'000 };
		};

		struct refreshed
		{
			bool tools_changed = false;
			bool resources_changed = false;
		};

		// Create and destroy on the main thread. `queue` and `registry` must outlive this object.
		mcp_bridge_host(cmd::command_registry& registry, cmd::main_thread_queue& queue, options opts = {});
		// Waits for the waiter thread; calls that are still waiting are answered with an error.
		~mcp_bridge_host() override;
		mcp_bridge_host(const mcp_bridge_host&) = delete;
		mcp_bridge_host& operator=(const mcp_bridge_host&) = delete;

		// Main thread. Rebuilds the snapshots from the registry; tells what changed since the last call.
		refreshed refresh();

		// May a command be offered to an agent in the read-only step?
		static bool is_read_only_tool(const cmd::command_desc& command);
		// "scene.node.create" -> "scene_node_create"; "x.y" at version 2 -> "x_y_v2".
		static std::string tool_name(const cmd::command_desc& command);

		// mcp::host: any thread.
		std::vector<mcp::tool_info> list_tools() override;
		void call_tool(std::string name, boost::json::value args, mcp::progress_sink progress, std::function<void(mcp::call_result)> done) override;
		std::vector<mcp::resource_info> list_resources() override;
		void read_resource(std::string uri, std::function<void(mcp::resource_result)> done) override;
		void log(mcp::level severity, std::string_view message) override;

	private:
		struct tool_entry
		{
			mcp::tool_info info;
			std::string command_id;
		};

		struct resource_entry
		{
			mcp::resource_info info;
			std::string command_id;
			// Argument of the command: the viewport id; empty for the log.
			std::string viewport;
		};

		struct pending
		{
			std::future<cmd::result> future;
			std::chrono::steady_clock::time_point deadline;
			std::function<void(cmd::result)> on_result;
			std::function<void(std::string)> on_failure;
		};

		void wait_for_main(std::future<cmd::result> future, std::function<void(cmd::result)> on_result, std::function<void(std::string)> on_failure);
		void run_waiter();
		mcp::call_result to_call_result(const cmd::result& result) const;

		cmd::command_registry& m_registry;
		cmd::main_thread_queue& m_queue;
		options m_options;

		// The snapshots; written by refresh() on the main thread, read from the io thread.
		mutable std::mutex m_snapshot_mutex;
		std::vector<tool_entry> m_tools;
		std::vector<resource_entry> m_resources;
		// What the snapshots were built from, to tell a change (main thread only).
		std::vector<std::string> m_tool_keys;
		std::vector<std::string> m_resource_keys;

		std::mutex m_wait_mutex;
		std::condition_variable m_wait_changed;
		std::deque<pending> m_waiting;
		std::atomic<bool> m_stop{ false };
		// Last member: started when everything above exists, joined first.
		std::thread m_waiter;
	};
}
