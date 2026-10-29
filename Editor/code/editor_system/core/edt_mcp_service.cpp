#include "edt_mcp_service.h"
#include <snk/common/logger/engine_log.h>
#include <string>
#include <utility>

namespace {
	constexpr int log_check_interval_frames = 30;

	mcp::server_config make_config(const edt::mcp_service::settings& opts)
	{
		mcp::server_config cfg;
		cfg.port = opts.port;
		cfg.connection_file = opts.connection_file;
		cfg.server_name = "snk-editor";
		cfg.server_version = "1";
		// The version of the command set an agent can rely on; bumped when a tool changes incompatibly.
		cfg.api_version = "1";
		cfg.instructions = "Snake Editor. Read-only: tools for reading the engine log, listing jobs and dialogs and taking screenshots; "
			"nothing here changes the scene or the files.";
		return cfg;
	}

	edt::mcp_bridge_host::options make_bridge_options(edt::mcp_service::settings& opts)
	{
		edt::mcp_bridge_host::options out;
		out.viewport_ids = std::move(opts.viewport_ids);
		return out;
	}
}

edt::mcp_service::mcp_service(cmd::command_registry& registry, ds::task_system& tasks, settings opts)
	: m_queue(registry, tasks)
	, m_bridge(registry, m_queue, make_bridge_options(opts))
	, m_server(m_bridge, make_config(opts))
{
}

edt::mcp_service::~mcp_service()
{
	m_server.stop();
}

bool edt::mcp_service::start()
{
	if (!m_server.start()) {
		egLOG_ERROR("Editor/MCP", "the agent server did not start: {}", m_server.error());
		return false;
	}
	m_log_seq = engine::log_journal_next_seq();
	egLOG_INFO("Editor/MCP", "agent server (read-only) at {}", m_server.url());
	return true;
}

void edt::mcp_service::pump()
{
	const mcp_bridge_host::refreshed changed = m_bridge.refresh();
	if (!m_server.running()) return;
	if (changed.tools_changed) m_server.notify_tools_changed();

	if (--m_frames_until_log_check > 0) return;
	m_frames_until_log_check = log_check_interval_frames;
	const std::uint64_t next = engine::log_journal_next_seq();
	if (next != m_log_seq) {
		m_log_seq = next;
		m_server.notify_resource_updated("snk://log");
	}
}
