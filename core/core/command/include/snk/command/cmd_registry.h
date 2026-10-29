#pragma once
#include "cmd_policy.h"
#include "cmd_registration.h"
#include "cmd_types.h"
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <snk/common/snk_api.h>

namespace cmd
{
	// JSON Schema of a command without arguments (and of a result that carries nothing).
	SNK_API boost::json::value no_args_schema();

	// The commands of a process in registration order, whoever asks for them: menus, shortcuts, tests, scripts,
	// agents. The engine, the game and the editor register into the same type.
	// Main thread only (the thread that created the registry), enforced by `invoke`. Commands may add and remove
	// registrations while they run. Other threads go through main_thread_queue.
	class SNK_API command_registry
	{
	public:
		command_registry();
		~command_registry();
		command_registry(const command_registry&) = delete;
		command_registry& operator=(const command_registry&) = delete;

		// Errors are logged and nothing is registered (the registration is not active): an empty id, no `invoke`,
		// a taken id, a schema check_schema refuses, an args_schema that is not "type":"object", version 0.
		[[nodiscard]] registration add(command_desc desc);

		bool contains(std::string_view id) const;
		std::size_t size() const;
		// Stays valid until the command is unregistered.
		const command_desc* find(std::string_view id) const;
		// False for an unknown id; true when the command has no `enabled`.
		bool is_enabled(std::string_view id) const;
		// Every command in registration order.
		std::vector<const command_desc*> list() const;

		// Fails, and the command does not run, if it does not exist, is disabled or the arguments do not match
		// its schema (see codes). A result that breaks result_schema is replaced by result_invalid.
		result invoke(std::string_view id, const call& request);
		result invoke(std::string_view id, boost::json::value args, origin from = origin::ui());

		bool on_main_thread() const noexcept { return std::this_thread::get_id() == m_main_thread; }

		// The policy for calls from agents (origin `agent`; the other origins are not asked): `invoke` asks the gate
		// before it checks the arguments or runs anything. Without a gate nothing is checked here. null removes it.
		// - allow: the command runs.
		// - deny: `denied`, the command does not run.
		// - ask: with a person to ask (`gate->can_ask()`) the answer is `confirmation_required` with a one-time ticket in
		//   `value["ticket"]`; without one (headless) it is `denied`. After a person approved the ticket, the same call
		//   (same command, the same arguments, the same agent session) with `call::confirmation` set to it runs once;
		//   any other call with it, a second use, or one before the approval is `denied`. The gate is asked again on
		//   that call, so a policy that turned to deny meanwhile wins.
		void set_policy_gate(std::shared_ptr<policy_gate> gate);

		// A person said yes to the ticket of a confirmation_required answer. False for a ticket that is unknown or
		// has expired.
		bool approve(std::string_view ticket);
		// Forgets a ticket (the person said no, or did not answer in time).
		void discard(std::string_view ticket);
		std::size_t pending_confirmations() const;

		// A ticket lives this long from its issue.
		static constexpr std::chrono::seconds confirmation_lifetime{ 120 };
		// An agent that asks again and again cannot fill the list: past this the call is `denied`.
		static constexpr std::size_t max_pending_confirmations = 64;
		// Tests: the time tickets expire by.
		void set_clock(std::function<std::chrono::steady_clock::time_point()> now);

	private:
		struct entry;

		struct confirmation_ticket
		{
			std::string id;
			std::string command;
			std::string args;
			std::string session;
			std::chrono::steady_clock::time_point expires;
			bool approved = false;
		};

		// nullopt: the call may go on.
		std::optional<result> apply_policy(const command_desc& command, const call& request);
		void drop_expired_tickets();
		std::chrono::steady_clock::time_point now() const;

		std::shared_ptr<policy_gate> m_gate;
		std::function<std::chrono::steady_clock::time_point()> m_clock;
		std::vector<confirmation_ticket> m_tickets;
		std::vector<std::shared_ptr<entry>> m_commands;
		std::uint64_t m_next_key = 1;
		std::thread::id m_main_thread = std::this_thread::get_id();
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
