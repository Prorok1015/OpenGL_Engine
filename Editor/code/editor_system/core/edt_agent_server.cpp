#include "edt_agent_server.h"
#include "edt_mcp_bridge_host.h"

#include <snk/common/logger/engine_log.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/mcp/mcp_connection_file.h>
#include <snk/mcp/mcp_server.h>

#include <string>
#include <utility>

std::filesystem::path edt::agent_project_dir(const std::filesystem::path& configured,
	const std::optional<std::filesystem::path>& project_root)
{
	if (!configured.empty()) {
		return configured;
	}
	return project_root ? *project_root : std::filesystem::current_path();
}

namespace {
	// Version of the command set an agent sees (serverInfo.snk_api, AI_INTEGRATION.md section 5): bump it when a
	// tool is renamed or changes its schema in a breaking way.
	constexpr const char* snk_api_version = "1";

	constexpr const char* instructions =
		"Read-only access to the Snake editor: the engine log, background jobs, open dialogs and screenshots of the "
		"scene and game views. A tool is a command of the editor; its name is the command id with '.' replaced by '_'.";

	mcp::server_config make_server_config(const edt::agent_server_options& options, std::string token)
	{
		mcp::server_config config;
		config.port = options.port;
		config.token = std::move(token);
		config.info.name = "snake-editor";
		config.info.version = "0";
		config.info.snk_api = snk_api_version;
		config.info.instructions = instructions;
		return config;
	}

	std::filesystem::path snk_dir_of(const edt::agent_server_options& options)
	{
		return options.project_dir / ".snk";
	}
}

// Members are destroyed bottom-up: the server first (its thread is joined), then the host it calls, then the queue.
struct edt::agent_server::impl
{
	impl(cmd::command_registry& commands, ds::task_system& tasks, agent_confirmations& confirmations, const agent_server_options& options_,
		std::string token_)
		: options(options_)
		, token(std::move(token_))
		, queue(commands, tasks)
		, bridge(commands, queue, tasks, confirmations)
		, server(bridge, make_server_config(options_, token))
	{
	}

	agent_server_options options;
	std::string token;
	cmd::main_thread_queue queue;
	mcp_bridge_host bridge;
	mcp::server server;
	mcp::connection_info file_info;
	bool file_written = false;
};

edt::agent_server::agent_server(cmd::command_registry& commands, ds::task_system& tasks, agent_confirmations& confirmations,
	agent_server_options options)
	: m_impl(std::make_unique<impl>(commands, tasks, confirmations, options, mcp::generate_token()))
{
	if (!m_impl->server.start()) {
		egLOG_ERROR("mcp", "the agent server did not start");
		return;
	}
	mcp::connection_info info;
	info.port = m_impl->server.port();
	info.token = m_impl->token;
	info.pid = mcp::current_process_id();
	// A server nobody can find the token of is no use, one whose token file cannot be protected is not safe, and a
	// project that another running editor already serves would have two editors fighting over one file.
	const std::string failure = mcp::write_connection_file(snk_dir_of(m_impl->options), info);
	if (!failure.empty()) {
		egLOG_ERROR("mcp", "the agent server is stopped: {}", failure);
		m_impl->server.stop();
		return;
	}
	m_impl->file_info = info;
	m_impl->file_written = true;
	egLOG_INFO("mcp", "agent server: {} (port and token in {})", info.url(),
		mcp::connection_file_path(snk_dir_of(m_impl->options)).string());
}

edt::agent_server::~agent_server()
{
	if (!m_impl) {
		return;
	}
	m_impl->server.stop();
	if (m_impl->file_written) {
		// Only our own file: another editor may have taken the project over since.
		mcp::remove_connection_file(snk_dir_of(m_impl->options), m_impl->file_info);
	}
}

bool edt::agent_server::running() const
{
	return m_impl->server.running();
}

std::uint16_t edt::agent_server::port() const
{
	return m_impl->server.port();
}
