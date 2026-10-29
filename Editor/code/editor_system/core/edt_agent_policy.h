#pragma once
#include <snk/command/cmd_policy.h>
#include <snk/command/cmd_types.h>
#include <snk/project/prj_json_text.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace edt
{
	class session_grants;

	// What an agent may do without asking, with a confirmation, or not at all (A6b #293, A6c #449; AI_INTEGRATION.md,
	// section 5). One mode per command category, overridable per command, in two layers that combine by "the stricter wins":
	//  - the project's policy, <project>/.snk/agent_policy.json, committed, written only by a person through the settings
	//    window (no command can write it, and .snk is mounted under no protocol);
	//  - the person's own settings, edt://settings/agent_policy.json, which can only tighten the project's.

	// The mode of a category when nothing says otherwise (the owner's decision of 2026-10-29): one table, easy to change.
	// A command with `risk` confirm or deny_by_default is at least ask / deny by default, and only an entry for that very
	// command in the project's file lifts it: an entry for its category does not. `live` is allowed here, but effective_mode
	// denies it whenever the live mode is off, whatever any layer says.
	constexpr cmd::policy_mode default_mode(cmd::agent_category category)
	{
		using enum cmd::agent_category;
		using cmd::policy_mode;
		switch (category) {
		case read: return policy_mode::allow;
		case scene_edit: return policy_mode::allow;
		case code_edit: return policy_mode::allow;
		case code_files: return policy_mode::ask;
		case cmake_presets: return policy_mode::ask;
		case build: return policy_mode::deny;
		case live: return policy_mode::allow;
		case unset:
		case not_for_agents: break;
		}
		return policy_mode::deny;
	}

	cmd::policy_mode default_mode(const cmd::command_desc& command);

	// The categories that can be set (not `unset` and `not_for_agents`).
	std::optional<cmd::agent_category> parse_settable_category(std::string_view name);
	std::optional<cmd::policy_mode> parse_policy_mode(std::string_view name);

	// One layer: a mode per category and per command. A command's own entry beats its category's.
	struct policy_layer
	{
		std::map<cmd::agent_category, cmd::policy_mode> categories;
		std::map<std::string, cmd::policy_mode> commands;

		// The command's own entry, else its category's held to the command's risk floor (confirm -> at least ask,
		// deny_by_default -> at least deny); nullopt: this layer says nothing.
		std::optional<cmd::policy_mode> resolve(const cmd::command_desc& command) const;
		bool empty() const { return categories.empty() && commands.empty(); }
	};

	struct policy_parse_result
	{
		policy_layer layer;
		// Empty: fine. Otherwise the layer is not usable (see agent_policy: fail closed).
		std::string error;
	};

	// {"version": 1, "categories": {"code_files": "allow"}, "commands": {"build.game_module": "ask"}}; the lists
	// "allow", "ask" and "deny" of command ids (AI_INTEGRATION.md, section 5) are read too. A bad value, an unknown category
	// or an unknown key is an error, never skipped: a typo must not widen the policy.
	policy_parse_result parse_policy_layer(std::string_view text);
	std::string serialize_policy_layer(const policy_layer& layer);

	// Where a decision comes from.
	enum class policy_source : std::uint8_t { fallback, default_table, project, personal };
	const char* to_string(policy_source source);

	struct effective_policy
	{
		cmd::policy_mode mode = cmd::policy_mode::deny;
		policy_source source = policy_source::fallback;
		// Why, in words for the agent and for a person.
		std::string reason;
	};

	// The mode of one command: the project layer (or the defaults) combined with the personal layer by the stricter.
	// `project_error` / `personal_error` (a layer that could not be read): everything is denied, reads included, so a
	// broken file never opens anything. `live_mode_on` false (the default: closed): a `live` command is denied before any
	// layer is read.
	effective_policy effective_mode(const cmd::command_desc& command, const policy_layer& project, const policy_layer& personal,
		const std::string& project_error = {}, const std::string& personal_error = {}, bool live_mode_on = false);

	// The two files, read again when they change on disk (a person edits them; an agent cannot).
	class agent_policy
	{
	public:
		agent_policy(std::filesystem::path project_file, std::filesystem::path personal_file);

		// Re-reads a file that changed; cheap otherwise.
		void refresh();
		// Main thread only (as the registry that calls the gate).
		effective_policy effective(const cmd::command_desc& command);

		// Whether the live mode is on: `live` commands are denied while it is not (and while nothing says it is).
		void set_live_mode(std::function<bool()> is_on) { m_live_on = std::move(is_on); }
		// Command ids a layer names that no command has get one warning per load (a typo, or a module not loaded yet).
		void set_known_commands(std::function<bool(std::string_view)> exists);

		const policy_layer& project() const { return m_project.layer; }
		const policy_layer& personal() const { return m_personal.layer; }
		const std::string& project_error() const { return m_project.error; }
		const std::string& personal_error() const { return m_personal.error; }
		const std::filesystem::path& project_file() const { return m_project.file; }
		const std::filesystem::path& personal_file() const { return m_personal.file; }

		// A person's edit (the settings window): writes the file atomically and takes the layer over. The error, nullopt on success.
		std::optional<std::string> save_project(const policy_layer& layer, const prj::text_writer& write = {});
		std::optional<std::string> save_personal(const policy_layer& layer, const prj::text_writer& write = {});

	private:
		struct source
		{
			std::filesystem::path file;
			policy_layer layer;
			std::string error;
			bool loaded = false;
			bool existed = false;
			// The last error of looking at the file, so a file that cannot be examined is warned about once.
			std::string stat_error;
			// The unknown command ids of this load were already warned about.
			bool warned_unknown = false;
			std::filesystem::file_time_type modified{};
			std::uintmax_t size = 0;
		};

		void reload(source& what);
		static bool changed_on_disk(const source& what);
		std::optional<std::string> save(source& what, const policy_layer& layer, const prj::text_writer& write);
		void warn_unknown_commands(source& what) const;

		source m_project;
		source m_personal;
		std::function<bool()> m_live_on;
		std::function<bool(std::string_view)> m_known;
		std::thread::id m_main_thread = std::this_thread::get_id();
	};

	// The registry's gate for agents: asks agent_policy. `ask` becomes a confirmation only when a person can be asked.
	class agent_policy_gate final : public cmd::policy_gate
	{
	public:
		explicit agent_policy_gate(agent_policy& policy) : m_policy(policy) {}

		cmd::policy_decision decide(const cmd::command_desc& command, const cmd::call& request) override;
		bool can_ask() const override { return m_can_ask; }
		void set_can_ask(bool value) { m_can_ask = value; }
		// "Allow until this agent session ends": an `ask` for a category the agent's session was granted is an allow.
		// A deny stays a deny. null: no grants.
		void set_session_grants(const session_grants* grants) { m_grants = grants; }

	private:
		agent_policy& m_policy;
		const session_grants* m_grants = nullptr;
		bool m_can_ask = false;
	};
}
