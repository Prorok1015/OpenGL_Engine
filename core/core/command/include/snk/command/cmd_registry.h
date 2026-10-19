#pragma once
#include "cmd_registration.h"
#include "cmd_types.h"
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
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

	private:
		struct entry;

		std::vector<std::shared_ptr<entry>> m_commands;
		std::uint64_t m_next_key = 1;
		std::thread::id m_main_thread = std::this_thread::get_id();
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
