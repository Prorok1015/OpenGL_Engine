#include "edt_agent_component.h"
#include "edt_agent_access.h"
#include "edt_agent_log_filter.h"
#include "edt_agent_model_guard.h"
#include "edt_agent_policy.h"
#include "edt_agent_server.h"
#include "edt_agent_server_settings.h"
#include "edt_data_protocol.h"
#include "edt_editor_layer.h"
#include "edt_path_text.h"
#include "edt_vcs_component.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <utility>

namespace
{
	// The MCP server for AI agents (read-only: log, jobs, dialogs, screenshots). Off unless the user turns it on.
	// "" (not set), true or false: an explicit false wins over the person's own switch (parse_cfg_switch).
	CFG_VAR_DEF_STRING(cfg_agent_server, "editor/agent_server", "");
	// MCP over stdin/stdout for one agent (--mcp-stdio, #452); main hands the saved stdout over as platform::protocol_stdout.
	CFG_VAR_DEF_BOOL(cfg_mcp_stdio, "editor/mcp_stdio", false);
	// 0: the operating system picks a free port; it is written to <project dir>/.snk/mcp.json with the token.
	CFG_VAR_DEF_INT(cfg_agent_server_port, "editor/agent_server_port", 0);
	// Minutes without a request after which an agent's MCP session is closed (its "allow until the end of the session" with it); 0: never.
	CFG_VAR_DEF_INT(cfg_agent_session_idle_minutes, "editor/agent_session_idle_minutes", 30);
	// Where .snk/mcp.json goes; empty: the open project's root, or the working directory without a project.
	CFG_VAR_DEF_STRING(cfg_project_dir, "editor/project_dir", "");
	// --agent-policy <file> (#452): a person's answers to the agent policy's `ask`, given in advance for a headless run
	// (agent_policy::set_preanswers_file). Relative to the working directory.
	CFG_VAR_DEF_PATH(cfg_agent_policy_file, "editor/agent_policy_file", "");
}

edt::agent_component::agent_component(ports from)
	: m_ports(std::move(from))
{
	// A throw below (the server, the policy files) must not leave this component's hooks in the registry, the log filter, the
	// vcs service and the layer: they name an object that is about to be gone, and a destructor does not run for it.
	try {
		build();
	}
	catch (...) {
		shutdown();
		m_ports.layer.unregister_implicit("edt/agent_confirmations");
		throw;
	}
}

void edt::agent_component::build()
{
	const bool headless = m_ports.headless;
	m_stdio = cfg_mcp_stdio;
	if (m_stdio && !m_ports.stdio_write) {
		egLOG_ERROR("mcp", "editor/mcp_stdio needs the --mcp-stdio option (stdout must belong to the protocol): no stdio transport");
		m_stdio = false;
	}
	// The policy and the server object always exist, so the person can switch the HTTP transport on later (#449). The
	// transport itself runs when cfg `editor/agent_server` says so (it wins, with its port) or the person's own switch
	// (edt://settings/agent_server.json) is on.
	agent_server_options options;
	const auto user_roots_for_server = m_ports.resources.mount_roots("edt");
	const std::filesystem::path settings_dir = (user_roots_for_server.empty() ? default_user_data_dir() : user_roots_for_server.back()) / "settings";
	m_settings_file = settings_dir / "agent_server.json";
	const agent_server_settings person_choice = load_agent_server_settings(m_settings_file);
	const std::string cfg_text = cfg_agent_server;
	const int cfg_port = cfg_agent_server_port;
	const agent_http_choice http = decide_agent_http(parse_cfg_switch(cfg_text), cfg_port, person_choice, headless, m_stdio);
	options.http = http.enabled;
	options.port = http.port;
	const int idle_minutes = cfg_agent_session_idle_minutes;
	// More than a week is the same as never, and keeps the minutes far from overflowing the clock's nanoseconds.
	constexpr int never_after_minutes = 7 * 24 * 60;
	options.session_idle_timeout = std::chrono::minutes{ (idle_minutes > never_after_minutes) ? 0 : std::max(idle_minutes, 0) };
	const std::string project_dir = cfg_project_dir;
	const auto agent_roots = m_ports.resources.mount_roots("edt");
	options.project_dir = agent_project_dir(project_dir, m_ports.project_root, agent_roots.empty() ? default_user_data_dir() : agent_roots.back());
	const std::filesystem::path project_policy = options.project_dir / ".snk" / "agent_policy.json";
	const auto user_roots = m_ports.resources.mount_roots("edt");
	const std::filesystem::path user_dir = user_roots.empty() ? default_user_data_dir() : user_roots.back();
	cmd::command_registry& commands = m_ports.commands;
	commands.set_agent_argument_check(make_agent_model_check(m_ports.resources));
	m_argument_checks = &commands;
	m_access = std::make_unique<agent_access>(commands, project_policy, user_dir / "settings" / "agent_policy.json", agent_confirmations::clock_function{},
		approved_snapshot_for(user_dir / "settings", options.project_dir));
	// What log.read gives anyone but the person at the editor (#935). The filter is the engine's, kept in the storage.
	ASSERT_MSG(commands.find("log.read") == nullptr || m_ports.log_filter != nullptr,
		"log.read is registered without its filter: an agent would read the whole journal");
	if (m_ports.log_filter != nullptr) {
		m_log_filter = m_ports.log_filter;
		install_agent_log_filter(*m_log_filter, [this, &commands] {
			return m_access != nullptr && agent_build_output_allowed(m_access->policy(), commands);
		}, agent_log_roots_of(options.project_dir));
	}
	// Asked at the start of every agent write, so an edit of either policy file applies at once.
	m_ports.vcs.follow_agent_policy(project_policy, [this] { return m_access != nullptr ? m_access->policy().effective_vcs() : vcs_policy{}; });
	m_access->policy().set_on_changed([this] { notify_tools_changed(); });
	m_access->policy().set_live_mode(m_ports.live_enabled);
	m_access->set_questions_routed(true);
	// A person is here to approve a looser project policy only with a window; headless never loosens (#483).
	m_access->policy().set_approval_allowed(!headless);
	// Once, so a project file that loosens something is known (the notice, the status bar) before an agent ever connects.
	if (!headless) m_access->policy().refresh();
	m_access->set_headless(headless);
	if (const std::filesystem::path preanswers = *cfg_agent_policy_file; !preanswers.empty()) {
		if (headless) {
			m_access->policy().set_preanswers_file(preanswers, options.project_dir);
		} else {
			egLOG_WARN("agent", "--agent-policy ignored: not headless ({})", utf8_of(preanswers));
		}
	}
	// Headless nobody reads a question, so the UI font is not checked (it has no Cyrillic there).
	m_ports.layer.register_implicit("edt/agent_confirmations", [this, headless] {
		if (m_access == nullptr) return false;
		if (m_server) m_server->tick();
		// Nothing to do for a person who does not use agents: no question can come, and the policy files are not read.
		if (m_server == nullptr || !m_server->active()) return true;
		return headless ? m_access->frame_headless() : m_access->frame(ImGui::GetFont());
	});
	// One signal for the end of an MCP session; whoever keeps something per session listens to it.
	editor_events& events = m_ports.context.events();
	m_connections.push_back(events.agent_session_closed.subscribe([this](const std::string& session) {
		if (m_access) m_access->confirmations().drop_session(session);
	}));
	m_connections.push_back(events.agent_session_closed.subscribe([this](const std::string& session) {
		if (m_ports.session_closed) m_ports.session_closed(session);
	}));
	options.on_session_closed = [&events](const std::string& session) { events.agent_session_closed.emit(session); };
	options.jobs = &m_ports.context.jobs();
	options.events = &events;
	ASSERT_MSG(m_log_filter != nullptr || commands.find("log.read") == nullptr,
		"the MCP bridge is built without the log filter: an agent would read the texts of its jobs as they are");
	options.log_filter = m_log_filter;
	m_quit.stdio_session = [this] { return m_server ? m_server->stdio_session() : std::string{}; };
	// Every open document, not only the scene: a modified preview is dropped too.
	m_quit.is_dirty = m_ports.any_unsaved;
	m_quit.discard_changes = m_ports.discard_changes;
	m_quit.request_stop = m_ports.request_stop;
	if (m_stdio) {
		options.stdio_write = m_ports.stdio_write;
		ds::task_system& tasks = m_ports.tasks;
		options.on_stdio_eof = [&tasks, alive = std::weak_ptr<int>(m_stdio_alive), headless, quit = m_quit] {
			tasks.post_main([alive, headless, quit] {
				if (alive.lock()) on_stdio_input_closed(headless, quit);
			});
		};
	}
	m_server = std::make_unique<agent_server>(commands, m_ports.tasks, *m_access, std::move(options));
}

edt::agent_component::~agent_component()
{
	shutdown();
}

void edt::agent_component::install(std::vector<registration>& out)
{
	if (editor_quit_available(m_stdio, m_ports.headless)) {
		registration added = register_editor_quit(m_ports.commands, std::move(m_quit));
		if (added.active()) out.push_back(std::move(added));
	}
}

void edt::agent_component::shutdown()
{
	// Its thread and the calls it queued use the registry and the task system: stop it before anything else goes.
	m_server.reset();
	m_connections.clear();
	if (m_log_filter) {
		// They name this system: nothing of it may be asked once it is gone, and without them an agent would read the
		// whole journal, so the registry's agents go first (the server above).
		m_log_filter->visible = [](std::string_view, const cmd::origin& from) { return from.kind == cmd::origin_kind::ui; };
		m_log_filter->redact = [](std::string_view text, const cmd::origin& from) {
			return from.kind == cmd::origin_kind::ui ? std::string(text) : std::string{};
		};
		m_log_filter = nullptr;
	}
	m_ports.vcs.follow_agent_policy({}, [] { return vcs_policy{}; });
	m_access.reset();
	if (m_argument_checks) {
		m_argument_checks->set_agent_argument_check(nullptr);
		m_argument_checks = nullptr;
	}
}

bool edt::agent_component::set_server(bool enabled, std::uint16_t port)
{
	if (!m_server) {
		return false;
	}
	bool done = true;
	if (enabled) {
		done = m_server->start_http(port);
	}
	else {
		m_server->stop_http();
	}
	if (!done) {
		return false;
	}
	if (const std::optional<std::string> error = save_agent_server_settings(m_settings_file, agent_server_settings{ enabled, port })) {
		egLOG_WARN("editor/agent_server", "the agent server choice cannot be saved: {}", *error);
	}
	return true;
}

void edt::agent_component::notify_tools_changed()
{
	if (m_server) m_server->notify_tools_changed();
}
