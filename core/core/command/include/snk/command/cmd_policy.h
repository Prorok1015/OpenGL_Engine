#pragma once
#include "cmd_types.h"
#include <cstdint>
#include <string>
#include <utility>

namespace cmd
{
	// What the policy says about one call. The order is the strictness: allow < ask < deny.
	enum class policy_mode : std::uint8_t { allow, ask, deny };

	// The stricter of two modes: how the project policy and a person's own settings combine (AI_INTEGRATION.md, section 5).
	constexpr policy_mode strictest(policy_mode a, policy_mode b)
	{
		return a > b ? a : b;
	}

	constexpr const char* to_string(policy_mode mode)
	{
		switch (mode) {
		case policy_mode::allow: return "allow";
		case policy_mode::ask: return "ask";
		case policy_mode::deny: return "deny";
		}
		return "deny";
	}

	struct policy_decision
	{
		policy_mode mode = policy_mode::deny;
		// Shown to the agent in the error, and to the person in a confirmation.
		std::string reason;

		static policy_decision allow() { return { policy_mode::allow, {} }; }
		static policy_decision ask(std::string why) { return { policy_mode::ask, std::move(why) }; }
		static policy_decision deny(std::string why) { return { policy_mode::deny, std::move(why) }; }
	};

	// Decides whether a call from an agent (origin `agent`) may run. Installed into the command_registry, which asks it
	// inside `invoke`, so no path of calling a command goes around it. Main thread only, like the registry.
	class policy_gate
	{
	public:
		virtual ~policy_gate() = default;

		virtual policy_decision decide(const command_desc& command, const call& request) = 0;

		// Whether a person can be asked at all. false (the default, and headless runs): an `ask` is refused as `denied`
		// instead of waiting for an answer that cannot come.
		virtual bool can_ask() const { return false; }
	};
}
