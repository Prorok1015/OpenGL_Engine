#include "edt_vcs_component.h"
#include "edt_curl_transport.h"
#include "edt_forge_account_model.h"
#include "edt_forge_account_panel.h"
#include "edt_git_panel.h"
#include "edt_github_provider.h"
#include <snk/config/cfg_variables.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_panel_registry.h>
#include <boost/json/value.hpp>
#include <string>
#include <utility>

namespace
{
	// The git program of version control (#454); empty: git on PATH.
	CFG_VAR_DEF_PATH(cfg_vcs_git, "vcs/git", "");

	edt::vcs_options vcs_options_of(const edt::vcs_component::ports& from)
	{
		edt::vcs_options options;
		options.git = *cfg_vcs_git;
		options.project_root = from.project_root;
		options.live_mode_on = from.live_mode_on;
		return options;
	}

	edt::forge_options forge_options_of(const std::shared_ptr<platform::secret_store>& secrets)
	{
		edt::forge_options options;
		options.provider = std::make_shared<edt::forge::github_provider>(edt::forge::make_curl_transport(edt::forge::configured_curl_options()));
		options.secrets = secrets;
		return options;
	}
}

edt::vcs_component::vcs_component(ports from)
	: m_ports(std::move(from))
	, m_secrets(std::make_shared<platform::secret_store>(platform::make_os_secret_backend()))
	, m_vcs(m_ports.context.jobs(), vcs_options_of(m_ports))
{
	vcs::repository_watcher::ports watch_ports;
	watch_ports.own_writes_running = [this] { return writes_running(); };
	m_watcher = std::make_unique<vcs::repository_watcher>(std::move(watch_ports));
	m_vcs.set_write_observer([this] { m_watcher->begin_own_write(); });
	m_forge = std::make_unique<forge_service>(m_vcs, forge_options_of(m_secrets));
}

edt::vcs_component::~vcs_component() = default;

void edt::vcs_component::install(std::vector<registration>& out)
{
	for (auto& command : register_vcs_commands(m_ports.context.actions().commands(), m_vcs)) {
		out.push_back(std::move(command));
	}
	for (auto& command : register_forge_commands(m_ports.context.actions().commands(), *m_forge)) {
		out.push_back(std::move(command));
	}
	// #458 PR-4: where a person sets the token of each host. The secret store's calls can block on an OS prompt, so they
	// run as jobs on the io lane; the job holds its own copies, nothing of it touches the panel.
	forge_account_model::ports account_ports;
	account_ports.secrets = m_secrets;
	account_ports.start_job = [this](std::string label, std::function<boost::json::value()> work) -> std::optional<std::uint64_t> {
		return m_ports.context.jobs().start_with_result(ds::lane::io, ds::task_desc{ std::move(label), "forge" }, m_account_jobs,
			[work = std::move(work)](ds::task_context&) -> boost::json::value { return work(); });
	};
	m_account_panel = std::make_shared<forge_account_panel>(m_ports.context.events(), forge_options{}.allowed_hosts, std::move(account_ports));
	out.push_back(m_ports.context.panels().add({ .id = m_account_panel->get_id(), .title = m_account_panel->get_title(),
		.slot = dock_slot::floating, .open_by_default = false, .panel = m_account_panel }));
	const auto open_account = [this] {
		m_ports.context.panels().set_open(m_account_panel->get_id(), true);
		m_account_panel->focus();
	};
	m_ports.git.set_open_account(open_account);
	m_ports.git.set_describe_target([this](const std::string& remote) { return m_forge->describe_target(remote); });
	out.push_back(register_forge_account_action(m_ports.context.actions(), open_account));
}

void edt::vcs_component::tick()
{
	// Not info(): after a project switch that would run the blocking repository probe here, every frame.
	m_watcher->follow(m_vcs);
	const vcs::repository_changes changes = m_watcher->tick();
	if (changes.refs) m_ports.context.events().repository_changed.emit(repository_change::refs);
	if (changes.index) m_ports.context.events().repository_changed.emit(repository_change::index);
}

void edt::vcs_component::follow_agent_policy(std::filesystem::path project_policy_file, std::function<vcs_policy()> source)
{
	m_vcs.set_policy_file(std::move(project_policy_file));
	m_vcs.set_policy_source(source);
	m_forge->set_policy_source(std::move(source));
}
