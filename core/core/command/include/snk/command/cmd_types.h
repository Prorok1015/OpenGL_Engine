#pragma once
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace cmd
{
	// Machine-readable outcomes of a command (result::code).
	namespace codes
	{
		inline constexpr const char* ok = "ok";
		// The arguments do not match args_schema.
		inline constexpr const char* invalid_args = "invalid_args";
		inline constexpr const char* disabled = "disabled";
		inline constexpr const char* not_found = "not_found";
		// A valid call that needs a decision first (e.g. discard_unsaved).
		inline constexpr const char* needs_input = "needs_input";
		// The command ran and could not do its job.
		inline constexpr const char* failed = "failed";
		// The state the call was built on has changed (a page cursor, later an expected generation): read again.
		inline constexpr const char* stale = "stale";
		// The command ran, but its result breaks result_schema.
		inline constexpr const char* result_invalid = "result_invalid";
	}

	// Not enforced by the registry yet: the policy that acts on it (confirmation, project allow-list) comes with
	// the MCP server.
	enum class danger : std::uint8_t { none, confirm, deny_by_default };

	enum class origin_kind : std::uint8_t { ui, test, agent, game };

	// Who asked for a command.
	struct origin
	{
		origin_kind kind = origin_kind::ui;
		std::string session; // agent only: the agent session the call belongs to

		static origin ui() { return {}; }
		static origin test() { return { origin_kind::test, {} }; }
		static origin agent(std::string session_id) { return { origin_kind::agent, std::move(session_id) }; }
		static origin game() { return { origin_kind::game, {} }; }

		bool operator==(const origin&) const = default;
	};

	struct call
	{
		// Must be an object for a command to run; anything else is invalid_args.
		boost::json::value args = boost::json::object{};
		origin from;
		// Carried for the document-generation check that arrives with mutating commands; not read yet.
		std::uint64_t expected_generation = 0;
	};

	struct result
	{
		bool ok = true;
		std::string code = codes::ok;
		boost::json::value value = boost::json::object{};
		std::string error;

		static result success(boost::json::value value = boost::json::object{}) { return { true, codes::ok, std::move(value), {} }; }
		static result failure(std::string code, std::string message) { return { false, std::move(code), boost::json::object{}, std::move(message) }; }
	};

	struct command_desc
	{
		std::string id;      // "file.save_as": stable, the dot separates the domain
		std::uint32_t version = 1; // a breaking schema change bumps it; starts at 1
		std::string summary; // one line for tool lists and hints
		// JSON Schema (see cmd_json_schema.h) of the arguments (a top-level "type":"object") and of the result.
		// Null: no_args_schema(). `invoke` of the registry validates the arguments before the command runs and
		// the result after it.
		boost::json::value args_schema;
		boost::json::value result_schema;
		danger risk = danger::none;
		bool mutates = false;
		bool long_running = false;
		// Listed for agents and scripts. False for placeholders that do nothing.
		bool expose_as_tool = true;
		std::function<result(const call&)> invoke;
		// Null: always enabled.
		std::function<bool()> enabled;
	};
}
