#pragma once
#include "edt_agent_confirmations.h"
#include "edt_agent_policy.h"
#include <snk/command/cmd_registry.h>
#include <filesystem>
#include <functional>
#include <memory>

struct ImFont;

namespace edt
{
	// Everything that decides what an agent may do, owned in one place: the two policy layers, the session grants, the
	// registry's gate and the confirmations a person answers. The gate is installed in the registry for as long as this
	// object lives.
	//
	// Create it on the main thread after the registry and destroy it before the registry; destroy the MCP server that
	// uses `confirmations()` first (its calls still waiting are answered with `denied` here).
	class agent_access
	{
	public:
		agent_access(cmd::command_registry& commands, std::filesystem::path project_policy_file, std::filesystem::path personal_policy_file);
		~agent_access();
		agent_access(const agent_access&) = delete;
		agent_access& operator=(const agent_access&) = delete;

		agent_policy& policy() { return m_policy; }
		agent_confirmations& confirmations() { return *m_confirmations; }
		session_grants& grants() { return m_grants; }

		// Once per frame, between windows (never inside one): re-reads a changed policy, decides whether a person can
		// be asked, times out what waited too long and draws the question. `font` is the UI font of the frame; without
		// it, or without Cyrillic in it, an `ask` is a `deny`. Returns true: it fits an implicit-layout registrar.
		bool frame(const ImFont* font);

		// Whether a person who can read the question is there. Headless it stays false: an `ask` is a `deny`. Losing the
		// person declines what waits.
		void set_person_present(bool present);

		// Whether the MCP bridge sends agent calls through confirmations(). Until it does an `ask` would only issue a
		// ticket nobody sees, so nobody is asked, whatever frame() finds: an `ask` is a `deny`.
		void set_questions_routed(bool routed) { m_routed = routed; }

	private:
		cmd::command_registry& m_commands;
		session_grants m_grants;
		agent_policy m_policy;
		std::shared_ptr<agent_policy_gate> m_gate;
		std::unique_ptr<agent_confirmations> m_confirmations;
		bool m_warned_about_font = false;
		bool m_routed = false;
		bool m_person_present = false;
	};
}
