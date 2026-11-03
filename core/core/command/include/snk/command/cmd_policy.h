#pragma once
#include "cmd_types.h"
#include <snk/common/snk_api.h>
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
		// An ask: the category it is for, which "allow for this session" grants. unset: the command's own.
		agent_category category = agent_category::unset;

		static policy_decision allow() { return { policy_mode::allow, {}, agent_category::unset }; }
		static policy_decision ask(std::string why, agent_category for_category = agent_category::unset)
		{
			return { policy_mode::ask, std::move(why), for_category };
		}
		static policy_decision deny(std::string why) { return { policy_mode::deny, std::move(why), agent_category::unset }; }
	};

	// The category the policy judges this call by, besides the command's own (the stricter of the two decides):
	// command_desc::category_of when the command has one and the arguments match args_schema, else
	// command_desc::category. Arguments that do not match never reach category_of; a category_of that throws gives
	// not_for_agents (the call is refused).
	SNK_API agent_category category_of_call(const command_desc& command, const call& request);

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
