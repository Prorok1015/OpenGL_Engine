#include "edt_agent_access.h"
#include <snk/common/logger/engine_log.h>
#include <imgui.h>
#include <utility>

edt::agent_access::agent_access(cmd::command_registry& commands, std::filesystem::path project_policy_file,
	std::filesystem::path personal_policy_file)
	: m_commands(commands)
	, m_policy(std::move(project_policy_file), std::move(personal_policy_file))
	, m_gate(std::make_shared<agent_policy_gate>(m_policy))
	, m_confirmations(std::make_unique<agent_confirmations>(commands, m_grants))
{
	m_gate->set_session_grants(&m_grants);
	m_policy.set_known_commands([&commands](std::string_view id) { return commands.find(id) != nullptr; });
	m_commands.set_policy_gate(m_gate);
}

edt::agent_access::~agent_access()
{
	// Answers what still waits (`denied`) while the gate and the registry are there, then takes the gate away before the
	// policy it points to goes.
	m_confirmations.reset();
	m_commands.set_policy_gate(nullptr);
}

bool edt::agent_access::frame(const ImFont* font)
{
	m_policy.refresh();

	const bool readable = font != nullptr && confirmation_glyphs_available(*font);
	if (!readable && !m_warned_about_font) {
		egLOG_ERROR("agent", "the UI font has no Cyrillic: nobody can be asked about an agent's request, so every `ask` is a `deny`");
		m_warned_about_font = true;
	}
	set_person_present(readable);

	m_confirmations->tick();
	return draw_agent_confirmations(*m_confirmations);
}

void edt::agent_access::set_person_present(bool present)
{
	m_gate->set_can_ask(present);
}
