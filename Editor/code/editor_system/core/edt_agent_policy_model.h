#pragma once
#include "edt_agent_policy.h"
#include <snk/command/cmd_policy.h>
#include <snk/command/cmd_types.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// Which of the two policy files the person is looking at (A6c #449).
	enum class policy_scope : std::uint8_t { project, personal };

	enum class vcs_list_key : std::uint8_t { push_branches, remotes, protected_branches };
	enum class vcs_flag_key : std::uint8_t { allow_hooks, allow_filters, allow_repo_credential_helper, allow_any_repo_config };

	struct policy_category_row
	{
		cmd::agent_category category = cmd::agent_category::read;
		// The entry of this scope's layer as edited; nullopt = "default" (no entry).
		std::optional<cmd::policy_mode> entry;
		// What an agent gets now, from the files as saved (both layers, the approval, the live switch).
		effective_policy effective;
		// The loosest mode this scope may set: the personal layer only tightens, so for it this is what the project alone gives.
		cmd::policy_mode loosest = cmd::policy_mode::allow;
	};

	struct policy_command_row
	{
		std::string id;
		std::string summary;
		std::optional<cmd::policy_mode> entry;
		effective_policy effective;
	};

	// The state of the agents window's policy part, without ImGui: the two layers as the person edits them, what an agent gets
	// from the files as saved, and the approval of a project file that loosens something (#483).
	//
	// Edits to the project layer are made on top of `approved_project()`, never on top of a file whose loosenings wait:
	// `save_project` approves what it writes, so a base that holds the file's pending loosenings would approve them
	// unseen. Applying while some wait therefore drops them from the file (the window says so).
	//
	// There is no command behind any of this, and none may be added: only a person at the window changes the policy.
	class agent_policy_model
	{
	public:
		using command_source = std::function<std::vector<const cmd::command_desc*>()>;

		using clock_fn = std::function<std::chrono::steady_clock::time_point()>;
		// How long the same layer has to be on screen, with the window used, before it can be approved: a button that appears
		// under the mouse (a layout shift, an agent's checkout changing the file) must not be clicked by accident.
		static constexpr std::chrono::milliseconds k_approve_arm_delay{ 1000 };

		// `now`: the clock of the arming delay (steady_clock by default).
		explicit agent_policy_model(agent_policy& policy, command_source commands = {}, clock_fn now = {});

		// Once per frame at the top of the window, and nowhere else while it draws (the rows read what this loaded): reads the
		// files again when they changed. An edit whose base changed on disk meanwhile is kept and flagged (`base_changed`); a
		// layer without edits simply follows the file.
		void refresh();
		// Takes a copy of the project layer and of its loosenings as they are now. The window calls it immediately before it
		// draws them and draws the copy (`displayed_loosenings`), so what `approve()` approves is exactly what was on screen.
		// A copy that differs from the last one restarts the arming.
		void mark_displayed();
		// Nothing waits any more: no copy, nothing to approve.
		void forget_displayed();

		// ---- the approval of a project file that loosens something
		bool pending() const;
		const std::vector<policy_loosening>& loosenings() const;
		const std::vector<policy_loosening>& displayed_loosenings() const { return m_displayed_loosenings; }
		// The person used the window (hovered or focused it) while the displayed copy was on screen.
		void note_interaction();
		// The same copy has been displayed for k_approve_arm_delay and the window was used since it appeared.
		bool approve_armed() const;
		// Approves the layer of the last mark_displayed(); refused when it is not armed, or when the file or its loosenings are
		// other by now. The error, nullopt on success.
		std::optional<std::string> approve();

		// ---- reading
		std::vector<policy_category_row> category_rows(policy_scope scope);
		std::vector<policy_command_row> command_rows(policy_scope scope, cmd::agent_category category);
		const policy_layer& layer(policy_scope scope) const;
		const std::string& error_of(policy_scope scope) const;
		// The layer has an edit that is not what the file says.
		bool dirty() const;
		bool dirty(policy_scope scope) const;
		// The file changed on disk under an edit that is not applied yet.
		bool base_changed(policy_scope scope) const;
		// Applying drops loosenings of the project file that wait (they are not in the edited layer).
		bool apply_drops_pending() const;

		// ---- editing; false: the scope may not do that (the personal layer cannot loosen)
		bool set_category(policy_scope scope, cmd::agent_category category, std::optional<cmd::policy_mode> mode);
		bool set_command(policy_scope scope, const std::string& id, std::optional<cmd::policy_mode> mode);
		bool set_vcs_list(policy_scope scope, vcs_list_key key, std::optional<std::vector<std::string>> value);
		bool set_vcs_flag(policy_scope scope, vcs_flag_key key, std::optional<bool> value);
		// The quick switch: ask before an agent creates, deletes or renames a code file (`code_files` ask, else allow).
		bool confirms_file_changes() const;
		void set_confirms_file_changes(bool confirm);

		// ---- apply / revert
		// Checks the edited layers (what the files will say must parse back), then writes the project layer and the personal
		// one atomically. The error, nullopt on success; on an error nothing is dropped.
		std::optional<std::string> apply();
		void revert();

	private:
		struct side
		{
			std::optional<policy_layer> edit;
			// The base as text when the edit began, to see the file move under it.
			std::string base_text;
			bool base_moved = false;
		};

		const policy_layer& base_of(policy_scope scope) const;
		policy_layer& edit_of(policy_scope scope);
		cmd::policy_mode loosest_of(cmd::agent_category category) const;
		static std::size_t index_of(policy_scope scope) { return scope == policy_scope::project ? 0 : 1; }

		agent_policy& m_policy;
		command_source m_commands;
		side m_side[2];
		clock_fn m_now;
		policy_layer m_displayed;
		std::vector<policy_loosening> m_displayed_loosenings;
		std::string m_displayed_key;
		std::chrono::steady_clock::time_point m_displayed_since{};
		bool m_has_displayed = false;
		bool m_interacted = false;
	};
}
