#pragma once
#include "edt_editor_quit.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cmd { class command_registry; struct log_read_filter; }
namespace ds { class task_system; }
namespace res { class resource_system; }

namespace edt
{
	class agent_access;
	class agent_server;
	class editor_context;
	class editor_layer;
	class vcs_component;

	// What an agent can reach, whole: the policy and the confirmations behind the MCP server (editor/agent_server,
	// editor/mcp_stdio), the server itself (HTTP and/or stdio), the filter of log.read, the argument check of #681, the editor.quit
	// command and the person's switch of the HTTP transport. editor_system creates it last of the agent pieces, at the end of init,
	// calls install(), and hands its access and server to the late-bound consumers (Play's agent mode, the migration's approvals,
	// live mode's tool list, the agents window). `shutdown()` runs first in ~editor_system: the server's thread uses the registry
	// and the task system, and the log filter names the editor system.
	class agent_component
	{
	public:
		struct ports
		{
			editor_context& context;
			res::resource_system& resources;
			cmd::command_registry& commands;
			ds::task_system& tasks;
			editor_layer& layer;
			vcs_component& vcs;
			// The engine's filter of log.read, in the app data storage; null when none (then log.read must not exist).
			cmd::log_read_filter* log_filter = nullptr;
			// The open project's root; nullopt without a project.
			std::optional<std::filesystem::path> project_root = std::nullopt;
			bool headless = false;
			// Writes a line to the protocol's stdout (--mcp-stdio); null when main handed none over.
			std::function<bool(std::string_view line)> stdio_write = {};
			// Live mode is on (the policy's category `live` is denied while it is off).
			std::function<bool()> live_enabled = {};
			// editor.quit: any open document has unsaved changes, and dropping them.
			std::function<bool()> any_unsaved = {};
			std::function<void()> discard_changes = {};
			std::function<void()> request_stop = {};
			// An MCP session ended: whoever keeps something per session drops it (the agent sessions of the scene edits).
			std::function<void(const std::string& session)> session_closed = {};
		};

		explicit agent_component(ports from);
		~agent_component();
		agent_component(const agent_component&) = delete;
		agent_component& operator=(const agent_component&) = delete;

		// editor.quit, where the editor supports it (a window with a person, or headless / stdio).
		void install(std::vector<registration>& out);
		// First in ~editor_system: the server (its thread and the calls it queued), the session listeners, the log filter's
		// closures (they name the editor system), the access and the argument check, in that order. Idempotent.
		void shutdown();

		// Null once shutdown() ran.
		agent_access* access() { return m_access.get(); }
		agent_server* server() { return m_server.get(); }
		// The person's switch for the agent server's HTTP transport (the agents window, #449): on (a new token and connection
		// file; a different port restarts it) or off (every session ends and is reported). The choice is kept in
		// edt://settings/agent_server.json, only when it took effect. False when the port cannot be bound.
		bool set_server(bool enabled, std::uint16_t port);
		const std::filesystem::path& settings_file() const { return m_settings_file; }
		// The tools an agent sees changed (live mode came on or went off, the policy changed).
		void notify_tools_changed();

	private:
		ports m_ports;
		bool m_stdio = false;
		// The editor.quit sources, kept for install().
		editor_quit_sources m_quit;
		// Taken by work the stdio transport posts to the main thread: it finds the component gone through the weak copy.
		std::shared_ptr<int> m_stdio_alive = std::make_shared<int>(0);
		std::filesystem::path m_settings_file;
		// Holds the check of #681 (make_agent_model_check over the resources) while this component lives.
		cmd::command_registry* m_argument_checks = nullptr;
		// The engine's filter of log.read, filled in here and emptied when the component goes.
		cmd::log_read_filter* m_log_filter = nullptr;
		std::unique_ptr<agent_access> m_access;
		// Listeners of editor_events::agent_session_closed; dropped before the access they reach.
		std::vector<connection> m_connections;
		// The MCP server (HTTP and/or stdio). After the access it asks, so it is destroyed first.
		std::unique_ptr<agent_server> m_server;
	};
}
