#include "edt_agent_ui_component.h"
#include "edt_agent_access.h"
#include "edt_agent_component.h"
#include "edt_agent_policy_model.h"
#include "edt_agent_server.h"
#include "edt_agent_server_settings.h"
#include "edt_agents_panel.h"
#include "edt_editor_layer.h"
#include "edt_ui_metrics.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/engine_assert.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <cstdint>
#include <format>
#include <utility>

edt::agent_ui_component::agent_ui_component(ports from)
	: m_ports(std::move(from))
{
	ASSERT_MSG(m_ports.agent.access() != nullptr, "the agents window needs the agent access");
	m_model = std::make_unique<agent_policy_model>(m_ports.agent.access()->policy(), [&commands = m_ports.commands] { return commands.list(); });
	agents_panel_services services;
	services.server = [this] { return m_ports.agent.server(); };
	services.set_server = [this](bool enabled, std::uint16_t port) { return m_ports.agent.set_server(enabled, port); };
	services.chosen_port = [this] { return load_agent_server_settings(m_ports.agent.settings_file()).port; };
	m_panel = std::make_shared<agents_panel>(*m_model, std::move(services));
	m_connections.push_back(m_ports.context.events().agent_action.subscribe([this](const agent_action_info& info) {
		m_recent_actions.push_back({ info.command, info.target, info.session, std::chrono::steady_clock::now() });
		while (m_recent_actions.size() > 8) m_recent_actions.pop_front();
	}));
	m_ports.layer.register_implicit("edt/agent_policy_notice", [this] {
		if (!m_ports.headless) draw_policy_notice();
		return true;
	});
}

edt::agent_ui_component::~agent_ui_component() = default;

void edt::agent_ui_component::install(std::vector<registration>& out)
{
	out.push_back(m_ports.context.panels().add({ .id = m_panel->get_id(), .title = m_panel->get_title(),
		.slot = dock_slot::floating, .open_by_default = false, .panel = m_panel }));
	out.push_back(m_ports.context.actions().add({ .id = "agent.settings.open", .label = "Agents...",
		.execute = no_args([this] { open_panel(); }),
		.expose_as_tool = false, .category = cmd::agent_category::not_for_agents }));
}

void edt::agent_ui_component::open_panel()
{
	m_ports.context.panels().set_open(m_panel->get_id(), true);
	m_panel->focus();
}

void edt::agent_ui_component::draw_policy_notice()
{
	if (m_ports.agent.access() == nullptr) return;
	if (!m_ports.agent.access()->policy().project_pending_approval()) {
		m_notice_dismissed_key.clear();
		return;
	}
	// agent_access::frame reads the files every frame while the server is active; a waiting policy must also notice, about once
	// a second, that its file was fixed or changed again. Nothing is done while no policy waits (the idle editor).
	if (m_ports.agent.server() == nullptr || !m_ports.agent.server()->active()) {
		const auto now = std::chrono::steady_clock::now();
		if (now - m_policy_refreshed >= std::chrono::seconds(1)) {
			m_policy_refreshed = now;
			m_ports.agent.access()->policy().refresh();
			if (!m_ports.agent.access()->policy().project_pending_approval()) return;
		}
	}
	std::string pending_key;
	for (const policy_loosening& change : m_ports.agent.access()->policy().pending_loosenings()) {
		pending_key += change.what + '|' + cmd::to_string(change.from) + '|' + cmd::to_string(change.to) + '\n';
	}
	if (m_notice_dismissed_key == pending_key) return;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(
		ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - em(12.f), viewport->WorkPos.y + viewport->WorkSize.y - em(12.f)),
		ImGuiCond_Always, ImVec2(1.f, 1.f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
	if (ImGui::Begin("Agent policy##agent_policy_notice", nullptr, flags)) {
		ImGui::TextUnformatted("The project's agent policy waits for your approval.");
		if (ImGui::Button("Open")) {
			open_panel();
			m_notice_dismissed_key = pending_key;
		}
		ImGui::SameLine();
		if (ImGui::Button("Later")) m_notice_dismissed_key = pending_key;
	}
	ImGui::End();
}

void edt::agent_ui_component::draw_status()
{
	if (m_ports.agent.access() == nullptr || m_ports.agent.server() == nullptr) return;
	const bool pending = m_ports.agent.access()->policy().project_pending_approval();
	// Before any ImGui call: for a person who does not use agents the menu bar is what it was.
	if (!pending && !m_ports.agent.server()->active()) return;

	const std::vector<agent_session_info> sessions = m_ports.agent.server()->sessions();
	const std::size_t connected = sessions.size() + (m_ports.agent.server()->stdio_session().empty() ? 0u : 1u);
	const std::size_t questions = m_ports.agent.access()->confirmations().pending().size();
	const bool flash = !m_recent_actions.empty()
		&& std::chrono::steady_clock::now() - m_recent_actions.back().at < std::chrono::seconds(2);

	std::string label = pending ? std::string("Agent policy: approve")
		: questions > 0 ? std::format("Agent: {} waiting", questions) : std::format("Agent: {}", connected);
	label += "###agent_status";
	const gui::theme_semantic_colors& colors = m_ports.context.colors();
	const ImVec4 color = pending || questions > 0 ? colors.agent
		: flash ? colors.success : ImGui::GetStyleColorVec4(ImGuiCol_Text);
	const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	const bool clicked = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(width, 0.f));
	ImGui::PopStyleColor();
	if (clicked) ImGui::OpenPopup("agent_status_popup");
	if (ImGui::BeginPopup("agent_status_popup")) {
		if (pending) ImGui::TextUnformatted("The project's agent policy waits for your approval.");
		if (m_recent_actions.empty()) {
			ImGui::TextDisabled("No agent action yet.");
		}
		for (auto it = m_recent_actions.rbegin(); it != m_recent_actions.rend(); ++it) {
			// Paths and ids of ours, but shown as plain text all the same.
			const std::string line = it->command + "  " + it->target;
			ImGui::TextUnformatted(line.c_str());
		}
		ImGui::Separator();
		for (const agent_session_info& session : sessions) {
			ImGui::PushID(session.id.c_str());
			ImGui::TextUnformatted(session.client_name.empty() ? "(unnamed)" : session.client_name.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Disconnect")) m_ports.agent.server()->close_session(session.id);
			ImGui::PopID();
		}
		if (ImGui::MenuItem("Settings...")) open_panel();
		ImGui::EndPopup();
	}
}
