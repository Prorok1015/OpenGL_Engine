#include "edt_agent_policy_model.h"
#include <algorithm>
#include <utility>

namespace edt
{
	namespace {

		cmd::command_desc probe_of(cmd::agent_category category)
		{
			cmd::command_desc probe;
			probe.id = "agent.settings.probe";
			probe.category = category;
			probe.summary = "the category as a whole";
			return probe;
		}

		bool agent_visible(const cmd::command_desc& command)
		{
			return command.expose_as_tool && command.category != cmd::agent_category::unset && command.category != cmd::agent_category::not_for_agents;
		}

	}

	agent_policy_model::agent_policy_model(agent_policy& policy, command_source commands, clock_fn now)
		: m_policy(policy)
		, m_commands(std::move(commands))
		, m_now(now ? std::move(now) : clock_fn([] { return std::chrono::steady_clock::now(); }))
	{
	}

	const policy_layer& agent_policy_model::base_of(policy_scope scope) const
	{
		if (scope == policy_scope::personal) return m_policy.personal();
		return m_policy.project_pending_approval() ? m_policy.approved_project() : m_policy.project();
	}

	const policy_layer& agent_policy_model::layer(policy_scope scope) const
	{
		const side& s = m_side[index_of(scope)];
		return s.edit ? *s.edit : base_of(scope);
	}

	policy_layer& agent_policy_model::edit_of(policy_scope scope)
	{
		side& s = m_side[index_of(scope)];
		if (!s.edit) {
			s.edit = base_of(scope);
			s.base_text = serialize_policy_layer(*s.edit);
			s.base_moved = false;
		}
		return *s.edit;
	}

	const std::string& agent_policy_model::error_of(policy_scope scope) const
	{
		return scope == policy_scope::project ? m_policy.project_error() : m_policy.personal_error();
	}

	void agent_policy_model::refresh()
	{
		m_policy.refresh();
		for (const policy_scope scope : { policy_scope::project, policy_scope::personal }) {
			side& s = m_side[index_of(scope)];
			if (!s.edit) {
				s.base_moved = false;
				continue;
			}
			const bool edited = serialize_policy_layer(*s.edit) != s.base_text;
			if (serialize_policy_layer(base_of(scope)) == s.base_text) continue;
			if (edited) {
				s.base_moved = true;
			}
			else {
				s.edit.reset();
				s.base_moved = false;
			}
		}
	}

	void agent_policy_model::mark_displayed()
	{
		m_displayed = m_policy.project();
		m_displayed_loosenings = m_policy.pending_loosenings();
		std::string key = serialize_policy_layer(m_displayed);
		for (const policy_loosening& change : m_displayed_loosenings) {
			key += '\n' + change.what + '|' + cmd::to_string(change.from) + '|' + cmd::to_string(change.to);
		}
		if (!m_has_displayed || key != m_displayed_key) {
			m_displayed_key = std::move(key);
			m_displayed_since = m_now();
			m_interacted = false;
		}
		m_has_displayed = true;
	}

	void agent_policy_model::note_review_opened()
	{
		if (!m_has_displayed || m_review_open) return;
		m_review_open = true;
		m_displayed_since = m_now();
		m_interacted = false;
	}

	void agent_policy_model::note_review_closed()
	{
		m_review_open = false;
		m_interacted = false;
	}

	void agent_policy_model::forget_displayed()
	{
		m_displayed = {};
		m_displayed_loosenings.clear();
		m_displayed_key.clear();
		m_has_displayed = false;
		m_interacted = false;
		m_review_open = false;
	}

	void agent_policy_model::note_interaction()
	{
		if (m_has_displayed && m_review_open) m_interacted = true;
	}

	bool agent_policy_model::approve_armed() const
	{
		return m_has_displayed && m_review_open && m_interacted && !too_many_to_review() && m_now() - m_displayed_since >= k_approve_arm_delay;
	}

	bool agent_policy_model::pending() const
	{
		return m_policy.project_pending_approval();
	}

	const std::vector<policy_loosening>& agent_policy_model::loosenings() const
	{
		return m_policy.pending_loosenings();
	}

	std::optional<std::string> agent_policy_model::approve()
	{
		if (!m_has_displayed) return "the policy has not been shown yet";
		if (too_many_to_review()) return "Too many changes to review here; edit the file down or approve smaller steps";
		if (!approve_armed()) return "look at the changes for a moment first: approving is possible once they have been on screen for a second";
		m_policy.refresh();
		const std::vector<policy_loosening>& now = m_policy.pending_loosenings();
		const bool same = now.size() == m_displayed_loosenings.size()
			&& std::equal(now.begin(), now.end(), m_displayed_loosenings.begin(), [](const policy_loosening& a, const policy_loosening& b) {
				return a.what == b.what && a.from == b.from && a.to == b.to;
			});
		if (!same) return "what the policy loosens changed since it was shown; review it again";
		std::optional<std::string> error = m_policy.approve_project_layer(m_displayed, &m_displayed_loosenings);
		if (!error) forget_displayed();
		return error;
	}

	bool agent_policy_model::dirty(policy_scope scope) const
	{
		const side& s = m_side[index_of(scope)];
		return s.edit && serialize_policy_layer(*s.edit) != s.base_text;
	}

	bool agent_policy_model::dirty() const
	{
		return dirty(policy_scope::project) || dirty(policy_scope::personal);
	}

	bool agent_policy_model::base_changed(policy_scope scope) const
	{
		return m_side[index_of(scope)].base_moved;
	}

	bool agent_policy_model::apply_drops_pending() const
	{
		return pending() && m_side[index_of(policy_scope::project)].edit.has_value();
	}

	cmd::policy_mode agent_policy_model::loosest_of(cmd::agent_category category) const
	{
		const cmd::command_desc probe = probe_of(category);
		return effective_mode(probe, base_of(policy_scope::project), policy_layer{}, m_policy.project_error(), {}, true).mode;
	}

	std::vector<policy_category_row> agent_policy_model::category_rows(policy_scope scope)
	{
		std::vector<policy_category_row> rows;
		const policy_layer& shown = layer(scope);
		for (const cmd::agent_category category : settable_categories()) {
			policy_category_row row;
			row.category = category;
			if (const auto found = shown.categories.find(category); found != shown.categories.end()) row.entry = found->second;
			row.effective = m_policy.effective_as_loaded(probe_of(category));
			row.loosest = scope == policy_scope::personal ? loosest_of(category) : cmd::policy_mode::allow;
			rows.push_back(std::move(row));
		}
		return rows;
	}

	std::vector<policy_command_row> agent_policy_model::command_rows(policy_scope scope, cmd::agent_category category)
	{
		std::vector<policy_command_row> rows;
		if (!m_commands) return rows;
		const policy_layer& shown = layer(scope);
		for (const cmd::command_desc* command : m_commands()) {
			if (command == nullptr || command->category != category || !agent_visible(*command)) continue;
			policy_command_row row;
			row.id = command->id;
			row.summary = command->summary;
			if (const auto found = shown.commands.find(command->id); found != shown.commands.end()) row.entry = found->second;
			row.effective = m_policy.effective_as_loaded(*command);
			rows.push_back(std::move(row));
		}
		std::ranges::sort(rows, [](const policy_command_row& a, const policy_command_row& b) { return a.id < b.id; });
		return rows;
	}

	bool agent_policy_model::set_category(policy_scope scope, cmd::agent_category category, std::optional<cmd::policy_mode> mode)
	{
		const auto allowed = [&] {
			const auto known = settable_categories();
			return std::ranges::find(known, category) != known.end();
		};
		if (!allowed()) return false;
		if (scope == policy_scope::personal && mode && *mode < loosest_of(category)) return false;
		policy_layer& edited = edit_of(scope);
		if (mode) {
			edited.categories[category] = *mode;
		}
		else {
			edited.categories.erase(category);
		}
		return true;
	}

	bool agent_policy_model::set_command(policy_scope scope, const std::string& id, std::optional<cmd::policy_mode> mode)
	{
		if (scope == policy_scope::personal && mode && m_commands) {
			for (const cmd::command_desc* command : m_commands()) {
				if (command == nullptr || command->id != id) continue;
				const cmd::policy_mode floor = effective_mode(*command, base_of(policy_scope::project), policy_layer{},
					m_policy.project_error(), {}, true).mode;
				if (*mode < floor) return false;
			}
		}
		policy_layer& edited = edit_of(scope);
		if (mode) {
			edited.commands[id] = *mode;
		}
		else {
			edited.commands.erase(id);
		}
		return true;
	}

	bool agent_policy_model::set_vcs_list(policy_scope scope, vcs_list_key key, std::optional<std::vector<std::string>> value)
	{
		// The same check parse_policy_layer makes (control or invisible characters, a comma, too long), so the edit saves.
		if (value) {
			for (const std::string& entry : *value) {
				if (entry.empty() || !vcs_entry_problem(entry).empty()) return false;
			}
		}
		policy_layer& edited = edit_of(scope);
		if (!edited.vcs) edited.vcs = vcs_policy_layer{};
		switch (key) {
		case vcs_list_key::push_branches: edited.vcs->push_branches = std::move(value); break;
		case vcs_list_key::remotes: edited.vcs->remotes = std::move(value); break;
		case vcs_list_key::protected_branches: edited.vcs->protected_branches = std::move(value); break;
		}
		if (*edited.vcs == vcs_policy_layer{}) edited.vcs.reset();
		return true;
	}

	bool agent_policy_model::set_vcs_flag(policy_scope scope, vcs_flag_key key, std::optional<bool> value)
	{
		// The personal layer may switch a flag off, never on (parse_policy_layer refuses it).
		if (scope == policy_scope::personal && value && *value) return false;
		policy_layer& edited = edit_of(scope);
		if (!edited.vcs) edited.vcs = vcs_policy_layer{};
		switch (key) {
		case vcs_flag_key::allow_hooks: edited.vcs->allow_hooks = value; break;
		case vcs_flag_key::allow_filters: edited.vcs->allow_filters = value; break;
		case vcs_flag_key::allow_repo_credential_helper: edited.vcs->allow_repo_credential_helper = value; break;
		case vcs_flag_key::allow_any_repo_config: edited.vcs->allow_any_repo_config = value; break;
		}
		if (*edited.vcs == vcs_policy_layer{}) edited.vcs.reset();
		return true;
	}

	bool agent_policy_model::confirms_file_changes() const
	{
		const policy_layer& shown = layer(policy_scope::project);
		const auto found = shown.categories.find(cmd::agent_category::code_files);
		const cmd::policy_mode mode = found != shown.categories.end() ? found->second : default_mode(cmd::agent_category::code_files);
		return mode == cmd::policy_mode::ask;
	}

	void agent_policy_model::set_confirms_file_changes(bool confirm)
	{
		set_category(policy_scope::project, cmd::agent_category::code_files,
			confirm ? std::nullopt : std::optional<cmd::policy_mode>{ cmd::policy_mode::allow });
	}

	std::optional<std::string> agent_policy_model::apply()
	{
		// Saving would replace what the file says now with an edit made on top of what it said before.
		for (const policy_scope scope : { policy_scope::project, policy_scope::personal }) {
			if (dirty(scope) && base_changed(scope)) {
				return std::string(scope == policy_scope::project ? "the project policy" : "your policy")
					+ " changed on disk while you were editing; revert to read it again";
			}
		}
		for (const policy_scope scope : { policy_scope::project, policy_scope::personal }) {
			if (!dirty(scope)) continue;
			const side& s = m_side[index_of(scope)];
			const policy_parse_result parsed = parse_policy_layer(serialize_policy_layer(*s.edit),
				scope == policy_scope::project ? policy_layer_kind::project : policy_layer_kind::personal);
			if (!parsed.error.empty()) {
				return std::string(scope == policy_scope::project ? "the project policy" : "your policy") + " cannot be saved: " + parsed.error;
			}
		}
		for (const policy_scope scope : { policy_scope::project, policy_scope::personal }) {
			if (!dirty(scope)) continue;
			side& s = m_side[index_of(scope)];
			// The project's save approves the command entries the person changed, not the ones the file brought along.
			const std::optional<std::string> error = scope == policy_scope::project
				? m_policy.save_project(*s.edit, {}, parse_policy_layer(s.base_text).layer)
				: m_policy.save_personal(*s.edit);
			if (error) return error;
			s.edit.reset();
			s.base_moved = false;
		}
		// An edit that came back to what the file says is no edit.
		for (side& s : m_side) {
			if (s.edit && serialize_policy_layer(*s.edit) == s.base_text) s.edit.reset();
		}
		return std::nullopt;
	}

	void agent_policy_model::revert()
	{
		for (side& s : m_side) {
			s.edit.reset();
			s.base_moved = false;
		}
	}
}
