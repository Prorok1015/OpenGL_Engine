#pragma once
#include <snk/command/cmd_policy.h>
#include <snk/command/cmd_types.h>
#include <snk/project/prj_json_text.h>
#include "edt_vcs_policy.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace edt
{
	class session_grants;

	// What an agent may do without asking, with a confirmation, or not at all (A6b #293, A6c #449; AI_INTEGRATION.md,
	// section 5). One mode per command category, overridable per command, in two layers that combine by "the stricter wins":
	//  - the project's policy, <project>/.snk/agent_policy.json, committed, written only by a person through the settings
	//    window (no command can write it, and .snk is mounted under no protocol);
	//  - the person's own settings, edt://settings/agent_policy.json, which can only tighten the project's.

	// The mode of a category when nothing says otherwise (the owner's decisions of 2026-10-29 and 2026-11-03): one table, easy to
	// change. `code_edit` asks: with live mode on, a change to a source file is built and loaded into the editor, so an agent that
	// edits code would run code without anyone being asked.
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
		case code_edit: return policy_mode::ask;
		case code_files: return policy_mode::ask;
		case cmake_presets: return policy_mode::ask;
		case build: return policy_mode::deny;
		case live: return policy_mode::allow;
		case editor_control: return policy_mode::allow;
		case play: return policy_mode::allow;
		// What a git write may touch is held by the hard rules of the "vcs" section (vcs_policy), whatever the mode.
		case vcs_read: return policy_mode::allow;
		case vcs_commit: return policy_mode::allow;
		case vcs_branch: return policy_mode::allow;
		case vcs_sync: return policy_mode::allow;
		case vcs_push: return policy_mode::allow;
		// Uses the stored token and the network: asks even for the reads (#458).
		case vcs_forge: return policy_mode::ask;
		case unset:
		case not_for_agents: break;
		}
		return policy_mode::deny;
	}

	cmd::policy_mode default_mode(const cmd::command_desc& command);
	// The same for one call judged by `category` (cmd::category_of_call), with the command's risk floor.
	cmd::policy_mode default_mode(const cmd::command_desc& command, cmd::agent_category category);

	// The categories that can be set (not `unset` and `not_for_agents`).
	std::optional<cmd::agent_category> parse_settable_category(std::string_view name);
	std::optional<cmd::policy_mode> parse_policy_mode(std::string_view name);

	// The "vcs" section of one layer (#455); a key the file does not set stays nullopt. How the two layers combine is
	// effective_vcs.
	struct vcs_policy_layer
	{
		std::optional<std::vector<std::string>> push_branches;
		std::optional<std::vector<std::string>> remotes;
		std::optional<std::vector<std::string>> protected_branches;
		std::optional<bool> allow_hooks;
		std::optional<bool> allow_filters;
		std::optional<bool> allow_repo_credential_helper;
		std::optional<bool> allow_any_repo_config;

		bool operator==(const vcs_policy_layer&) const = default;
	};

	// One layer: a mode per category and per command. A command's own entry beats its category's.
	struct policy_layer
	{
		std::map<cmd::agent_category, cmd::policy_mode> categories;
		std::map<std::string, cmd::policy_mode> commands;
		std::optional<vcs_policy_layer> vcs;

		// The command's own entry, else its category's held to the command's risk floor (confirm -> at least ask,
		// deny_by_default -> at least deny); nullopt: this layer says nothing.
		std::optional<cmd::policy_mode> resolve(const cmd::command_desc& command) const;
		// The same for a call judged by `category` (cmd::category_of_call) instead of the command's own: the command's
		// own entry counts only when `category` is the command's category.
		std::optional<cmd::policy_mode> resolve(const cmd::command_desc& command, cmd::agent_category category) const;
		bool empty() const { return categories.empty() && commands.empty() && !vcs; }
	};

	struct policy_parse_result
	{
		policy_layer layer;
		// Empty: fine. Otherwise the layer is not usable (see agent_policy: fail closed).
		std::string error;
	};

	// The personal layer may only tighten: a "vcs" flag set to true there is an error of that layer. The --agent-policy file
	// (`preanswers`, see apply_preanswers) has no "vcs" section at all: the git rules come from the two layers only.
	enum class policy_layer_kind : std::uint8_t { project, personal, preanswers };

	// {"version": 1, "categories": {"code_files": "allow"}, "commands": {"build.game_module": "ask"}, "vcs": {...}}; the
	// lists "allow", "ask" and "deny" of command ids (AI_INTEGRATION.md, section 5) are read too. "vcs" holds
	// "push_branches", "remotes", "protected_branches" (lists of strings) and "allow_hooks", "allow_filters",
	// "allow_repo_credential_helper", "allow_any_repo_config" (booleans), each optional. A bad value, an unknown category
	// or an unknown key is an error, never skipped: a typo must not widen the policy.
	policy_parse_result parse_policy_layer(std::string_view text, policy_layer_kind kind = policy_layer_kind::project);
	std::string serialize_policy_layer(const policy_layer& layer);

	// Where a decision comes from.
	enum class policy_source : std::uint8_t { fallback, default_table, project, personal, approval_pending, preanswers };
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
	// One call judged by `category` (cmd::category_of_call). A command that is unset or not_for_agents stays denied,
	// whatever category its call has.
	effective_policy effective_mode_of_call(const cmd::command_desc& command, cmd::agent_category category, const policy_layer& project,
		const policy_layer& personal, const std::string& project_error = {}, const std::string& personal_error = {},
		bool live_mode_on = false);

	// The --agent-policy file (#452): a person's answers to `ask` given in advance for a headless run. It is not
	// a policy layer: on top of `decided` (effective_mode_of_call of the two layers) it may turn an `ask` into `allow` (its
	// command's own entry, or its category's for a command without risk) and tighten anything, never lift a deny (of a layer,
	// of the defaults or a risk, of `unset`/`not_for_agents`, of the live switch). A file that cannot be used
	// (`preanswers_error`) denies everything. The only place the file decides anything.
	effective_policy apply_preanswers(const cmd::command_desc& command, cmd::agent_category category, effective_policy decided,
		const policy_layer& preanswers, const std::string& preanswers_error);

	// What a changed project layer makes looser than the layer a person approved (#483). `to` is the worst case when the
	// new layer only drops an entry (the mode it falls back to is not known here).
	struct policy_loosening
	{
		std::string what;
		cmd::policy_mode from = cmd::policy_mode::allow;
		cmd::policy_mode to = cmd::policy_mode::allow;
	};

	// Every way `current` lets an agent do more than `approved` does: per category (also for the commands that carry a risk
	// floor), per command of `commands`, and per entry for an id no command has (judged without knowing its category: a new
	// or looser entry, or a dropped one that was not `allow`, counts). Conservative on purpose: an answer of "nothing" is what
	// lets a changed file take over without a person, so it must never miss a loosening. A field added to policy_layer that can
	// loosen has to be compared here.
	std::vector<policy_loosening> find_loosenings(const policy_layer& approved, const policy_layer& current,
		const std::vector<const cmd::command_desc*>& commands);

	// Where the layer a person approved for a project is kept: <personal_settings_dir>/agent_policy_approved/<hash>.json, and
	// the canonical path ("key") the copy was approved for, written inside it and checked on load (a hash that two paths share
	// must not hand one project the approval of another). A moved project is a new project: it is approved again.
	struct approved_snapshot_location
	{
		std::filesystem::path file;
		std::string key;
	};
	approved_snapshot_location approved_snapshot_for(const std::filesystem::path& personal_settings_dir, const std::filesystem::path& project_root);

	// The git rules of the two layers: push_branches and remotes are the project's lists (else the defaults of vcs_policy),
	// narrowed by the personal layer's to the entries both name; protected_branches are main, master and both layers'
	// entries; a flag is on only when the project turns it on and the personal layer does not turn it off. A layer that
	// cannot be used: no push and no remote at all, every flag off.
	vcs_policy effective_vcs(const policy_layer& project, const policy_layer& personal, const std::string& project_error = {},
		const std::string& personal_error = {});

	// The two files, read again when they change on disk (a person edits them; an agent cannot).
	//
	// The project file lives in the work tree, so whatever changes the tree (a checkout, a pull, a tool) changes the policy
	// an agent is judged by. A change that lets an agent do more therefore waits for a person: until one approves it, every
	// decision is the stricter of the approved layer and the file as it is now (a change that only tightens, or changes
	// nothing, needs no one and takes over by itself). The approved layer is a copy kept next to the personal settings; a
	// project without one is approved against the built-in defaults, i.e. an empty layer.
	class agent_policy
	{
	public:
		// `approved`: where the approved copy is kept (empty file: in memory only, as in tests). The copy is trusted because no
		// command, tool or agent can write into edt://settings; one that could would approve its own loosenings.
		agent_policy(std::filesystem::path project_file, std::filesystem::path personal_file, approved_snapshot_location approved = {});

		// Re-reads a file that changed; cheap otherwise.
		void refresh();
		// Main thread only (as the registry that calls the gate).
		effective_policy effective(const cmd::command_desc& command);
		// The rules of the "vcs" sections, read again like effective(). Main thread only.
		vcs_policy effective_vcs();
		// One call judged by `category` (cmd::category_of_call).
		effective_policy effective(const cmd::command_desc& command, cmd::agent_category category);

		// Whether the live mode is on: `live` commands are denied while it is not (and while nothing says it is).
		void set_live_mode(std::function<bool()> is_on) { m_live_on = std::move(is_on); }
		// The --agent-policy file (apply_preanswers), read once, now; empty: none. A change while the editor runs is ignored
		// (one WARN). A file that is missing, cannot be used, or lies inside `project_root` or the git worktree holding it
		// (the nearest folder up from the project with a `.git` entry) is an ERROR and closes everything while it applies.
		void set_preanswers_file(std::filesystem::path file, std::filesystem::path project_root);
		// A headless editor (agent_access::set_headless). The --agent-policy file applies only there; with a window it never
		// does, whether a person can be asked or not.
		void set_headless(bool headless);
		bool headless() const { return m_headless; }
		// Command ids a layer names that no command has get one warning per load (a typo, or a module not loaded yet).
		void set_known_commands(std::function<bool(std::string_view)> exists);
		// The commands of the registry, for judging what a changed project file loosens. Without it only the categories and the
		// ids named in the layers are compared (never less strict).
		// `generation` (cmd::command_registry::generation) says whether the set of commands changed since the file was judged
		// (a module reloaded with a risk floor an entry of the file would lift); without it only its size is compared.
		void set_command_source(std::function<std::vector<const cmd::command_desc*>()> commands,
			std::function<std::uint64_t()> generation = {});
		// Whether a person is here to approve (a UI session; never headless). Without one approve_project_layer refuses and a
		// loosening never takes effect.
		void set_approval_allowed(bool allowed) { m_approval_allowed = allowed; }
		// The project file as it is now loosens something an agent may do and no person has approved it.
		bool project_pending_approval() const { return m_pending; }
		const std::vector<policy_loosening>& pending_loosenings() const { return m_loosenings; }
		// The layer a person approved last (empty before any).
		const policy_layer& approved_project() const { return m_approved; }
		// The person approves the project file, which must still be the `reviewed` layer they were shown: a file that changed
		// in between (a checkout, an agent's tool) is refused and has to be looked at again. The error, nullopt on success.
		std::optional<std::string> approve_project_layer(const policy_layer& reviewed);

		// Called (main thread) after a layer that was already loaded is read again, because its file changed or a person saved it:
		// what an agent may see can have changed, so the MCP clients are told to ask for the tool list again.
		void set_on_changed(std::function<void()> on_changed) { m_on_changed = std::move(on_changed); }

		const policy_layer& project() const { return m_project.layer; }
		const policy_layer& personal() const { return m_personal.layer; }
		const std::string& project_error() const { return m_project.error; }
		const std::string& personal_error() const { return m_personal.error; }
		const std::filesystem::path& project_file() const { return m_project.file; }
		const std::filesystem::path& personal_file() const { return m_personal.file; }
		const policy_layer& preanswers() const { return m_preanswers.layer; }
		const std::string& preanswers_error() const { return m_preanswers.error; }
		bool has_preanswers() const { return !m_preanswers.file.empty(); }

		// A person's edit (the settings window): writes the file atomically, takes the layer over and approves all of it. The
		// window therefore builds `layer` from approved_project() plus the person's edits, never from the file as it is while
		// loosenings wait: whatever pending loosening is in `layer` is approved with it, and one that is not is dropped from
		// the file. The error, nullopt on success.
		std::optional<std::string> save_project(const policy_layer& layer, const prj::text_writer& write = {});
		std::optional<std::string> save_personal(const policy_layer& layer, const prj::text_writer& write = {});

	private:
		struct source
		{
			std::filesystem::path file;
			policy_layer_kind kind = policy_layer_kind::project;
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
		// Compares the project file with the approved layer: takes it over when it loosens nothing, else it is pending.
		void reconcile_project();
		std::optional<std::string> keep_approved(const policy_layer& layer);
		void load_approved();
		// The commands changed since the project file was last judged: judges it again.
		void rejudge_if_commands_changed();
		std::vector<const cmd::command_desc*> commands() const;
		std::optional<std::string> save(source& what, const policy_layer& layer, const prj::text_writer& write, bool approve = false);
		void warn_unknown_commands(source& what) const;

		source m_project;
		source m_personal;
		source m_preanswers;
		std::filesystem::path m_preanswers_project_root;
		bool m_warned_preanswers_change = false;
		bool m_headless = false;
		std::function<bool()> m_live_on;
		std::function<bool(std::string_view)> m_known;
		std::function<void()> m_on_changed;
		std::function<std::vector<const cmd::command_desc*>()> m_commands;
		std::filesystem::path m_approved_file;
		std::string m_approved_key;
		policy_layer m_approved;
		// The approved copy exists but could not be used (kept next to it as *.corrupt): what a person had approved is unknown,
		// so nothing is adopted silently until they approve again.
		bool m_snapshot_lost = false;
		std::function<std::uint64_t()> m_generation;
		std::uint64_t m_reconciled_commands = static_cast<std::uint64_t>(-1);
		std::vector<policy_loosening> m_loosenings;
		bool m_pending = false;
		bool m_approval_allowed = false;
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
