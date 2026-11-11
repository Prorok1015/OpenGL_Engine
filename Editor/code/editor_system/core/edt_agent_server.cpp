#include "edt_agent_server.h"
#include "edt_agent_access.h"
#include "edt_mcp_bridge_host.h"

#include <snk/common/logger/engine_log.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/mcp/mcp_connection_file.h>
#include <snk/mcp/mcp_server.h>
#include <snk/mcp/mcp_stdio.h>

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

std::filesystem::path edt::agent_project_dir(const std::filesystem::path& configured,
	const std::optional<std::filesystem::path>& project_root, const std::filesystem::path& without_project)
{
	if (!configured.empty()) {
		return configured;
	}
	return project_root ? *project_root : without_project;
}

namespace {
	// Version of the command set an agent sees (serverInfo.snk_api, AI_INTEGRATION.md section 5): bump it when a
	// tool is renamed or changes its schema in a breaking way.
	constexpr const char* snk_api_version = "2";

	constexpr const char* instructions =
		"Access to the Snake editor, as far as the person's agent policy allows: reading the engine log, jobs, dialogs, "
		"the scene, the problems and the git state, taking screenshots, and (when the policy allows) editing the scene, "
		"code files and building. Some tools ask the person for a confirmation before they run, and a call can wait for "
		"the answer or for the person to finish what they are doing; a refused call says why. A tool is a command of the "
		"editor; its name is the command id with '.' replaced by '_'. The list of tools changes with the policy "
		"and with the person's switches (live mode): ask for it again after a tools/list_changed notification.";

	mcp::server_info make_server_info()
	{
		mcp::server_info info;
		info.name = "snake-editor";
		info.version = "0";
		info.snk_api = snk_api_version;
		info.instructions = instructions;
		return info;
	}

	edt::mcp_bridge_options make_bridge_options(const edt::agent_server_options& options)
	{
		edt::mcp_bridge_options out;
		out.jobs = options.jobs;
		out.events = options.events;
		return out;
	}

	mcp::server_config make_server_config(const edt::agent_server_options& options, std::string token)
	{
		mcp::server_config config;
		config.port = options.port;
		config.token = std::move(token);
		config.session_idle_timeout = options.session_idle_timeout;
		config.info = make_server_info();
		return config;
	}

	std::filesystem::path snk_dir_of(const edt::agent_server_options& options)
	{
		return options.project_dir / ".snk";
	}
}

// Members are destroyed bottom-up: the transports first (their threads are joined), then the host they call.
struct edt::agent_server::impl
{
	impl(cmd::command_registry& commands, ds::task_system& tasks, agent_access& access, const agent_server_options& options_)
		: options(options_)
		, bridge(commands, tasks, access, options_.on_session_closed, make_bridge_options(options_))
	{
	}

	agent_server_options options;
	std::string token;
	mcp_bridge_host bridge;
	std::unique_ptr<mcp::server> server;
	std::unique_ptr<mcp::stdio_transport> stdio;
	mcp::connection_info file_info;
	bool file_written = false;
};

edt::agent_server::agent_server(cmd::command_registry& commands, ds::task_system& tasks, agent_access& access, agent_server_options options)
	: m_impl(std::make_unique<impl>(commands, tasks, access, options))
{
	if (m_impl->options.stdio_write) {
		mcp::stdio_config config;
		config.info = make_server_info();
		config.write = m_impl->options.stdio_write;
		config.on_eof = m_impl->options.on_stdio_eof;
		m_impl->stdio = std::make_unique<mcp::stdio_transport>(m_impl->bridge, std::move(config));
		m_impl->stdio->start(mcp::make_stdin_source());
		egLOG_INFO("mcp", "agent server: MCP over stdin/stdout");
	}
	if (m_impl->options.http) {
		bring_up_http(m_impl->options.port);
	}
}

bool edt::agent_server::start_http(std::uint16_t port)
{
	if (running() && m_impl->options.port == port) {
		return true;
	}
	return bring_up_http(port);
}

void edt::agent_server::stop_http()
{
	end_all_http_sessions();
	if (m_impl->server) {
		m_impl->server->stop();
		m_impl->server.reset();
	}
	if (m_impl->file_written) {
		if (const std::string problem = mcp::remove_connection_file(snk_dir_of(m_impl->options), m_impl->file_info); !problem.empty()) {
			egLOG_WARN("mcp", "{}", problem);
		}
		m_impl->file_written = false;
	}
	m_impl->options.http = false;
}

void edt::agent_server::end_all_http_sessions()
{
	if (!m_impl->server) {
		return;
	}
	std::vector<std::string> ids;
	for (const mcp::session_info& found : m_impl->server->sessions()) {
		ids.push_back(found.id);
	}
	// Before the transport goes: a call of theirs that is already queued for the main thread must not run after this.
	m_impl->bridge.end_sessions(ids);
}

std::vector<edt::agent_session_info> edt::agent_server::sessions() const
{
	std::vector<agent_session_info> out;
	if (!m_impl->server) {
		return out;
	}
	for (mcp::session_info& found : m_impl->server->sessions()) {
		out.push_back({ std::move(found.id), std::move(found.client_name), found.age, found.idle, found.listening });
	}
	return out;
}

bool edt::agent_server::close_session(const std::string& id)
{
	if (!m_impl->server) {
		return false;
	}
	m_impl->bridge.end_sessions({ id });
	return m_impl->server->close_session(id);
}

bool edt::agent_server::active() const
{
	return running() || m_impl->stdio != nullptr;
}

// The new server binds its port first; only when that worked does it replace the old one, so a port that is taken leaves
// the running server (and its connection file) as it was.
bool edt::agent_server::bring_up_http(std::uint16_t port)
{
	agent_server_options wanted = m_impl->options;
	wanted.port = port;
	std::string token = mcp::generate_token();
	auto fresh = std::make_unique<mcp::server>(m_impl->bridge, make_server_config(wanted, token));
	if (!fresh->start()) {
		egLOG_ERROR("mcp", "the agent server did not start on port {}{}", port,
			running() ? std::format("; it keeps running on port {}", this->port()) : std::string{});
		return false;
	}
	mcp::connection_info info;
	info.port = fresh->port();
	info.token = token;
	info.pid = mcp::current_process_id();
	// A server nobody can find the token of is no use, one whose token file cannot be protected is not safe, and a
	// project that another running editor already serves would have two editors fighting over one file.
	const std::string failure = mcp::write_connection_file(snk_dir_of(wanted), info);
	if (!failure.empty()) {
		egLOG_ERROR("mcp", "the agent server is stopped: {}", failure);
		fresh->stop();
		return false;
	}
	if (m_impl->server) {
		end_all_http_sessions();
		m_impl->server->stop();
	}
	m_impl->server = std::move(fresh);
	m_impl->token = std::move(token);
	m_impl->options = std::move(wanted);
	m_impl->options.http = true;
	m_impl->file_info = info;
	m_impl->file_written = true;
	egLOG_INFO("mcp", "agent server: {} (port and token in {})", info.url(),
		mcp::connection_file_path(snk_dir_of(m_impl->options)).string());
	return true;
}

edt::agent_server::~agent_server()
{
	if (!m_impl) {
		return;
	}
	// The sessions end with the transports; the editor is going down with them, nobody is left to tell.
	m_impl->bridge.stop_reporting();
	if (m_impl->stdio) {
		m_impl->stdio->stop();
	}
	if (m_impl->server) {
		m_impl->server->stop();
	}
	if (m_impl->file_written) {
		// Only our own file: another editor may have taken the project over since.
		mcp::remove_connection_file(snk_dir_of(m_impl->options), m_impl->file_info);
	}
}

bool edt::agent_server::running() const
{
	return m_impl->server && m_impl->server->running();
}

std::uint16_t edt::agent_server::port() const
{
	return m_impl->server ? m_impl->server->port() : std::uint16_t{ 0 };
}

std::string edt::agent_server::stdio_session() const
{
	return m_impl->stdio ? m_impl->stdio->session_id() : std::string{};
}

void edt::agent_server::tick()
{
	m_impl->bridge.tick();
}

void edt::agent_server::notify_tools_changed()
{
	if (running()) {
		m_impl->server->notify_tools_changed();
	}
	if (m_impl->stdio) {
		m_impl->stdio->notify_tools_changed();
	}
}
