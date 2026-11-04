#include "edt_agents_panel.h"
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace {
	const ImVec4 k_error_color{ 1.f, 0.3f, 0.3f, 1.f };
	const ImVec4 k_warning_color{ 1.f, 0.8f, 0.f, 1.f };
	const ImVec4 k_ok_color{ 0.4f, 0.9f, 0.4f, 1.f };
	constexpr std::size_t k_banner_line_bytes = 120;
	constexpr const char* k_review_popup = "agent_policy_review";

	const char* label_of(cmd::agent_category category)
	{
		return cmd::to_string(category);
	}

	// One combo for "default / allow / ask / deny"; a mode looser than `loosest` is shown but cannot be chosen. True: changed.
	bool mode_combo(const char* id, std::optional<cmd::policy_mode>& value, cmd::policy_mode loosest)
	{
		bool changed = false;
		const char* shown = value ? cmd::to_string(*value) : "default";
		ImGui::SetNextItemWidth(100.f);
		if (ImGui::BeginCombo(id, shown)) {
			if (ImGui::Selectable("default", !value.has_value())) {
				value.reset();
				changed = true;
			}
			for (const cmd::policy_mode mode : { cmd::policy_mode::allow, cmd::policy_mode::ask, cmd::policy_mode::deny }) {
				const bool blocked = mode < loosest;
				ImGui::BeginDisabled(blocked);
				if (ImGui::Selectable(cmd::to_string(mode), value == mode)) {
					value = mode;
					changed = true;
				}
				ImGui::EndDisabled();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	std::string seconds_text(std::chrono::seconds age)
	{
		const long long total = age.count();
		if (total < 60) return std::format("{} s", total);
		if (total < 3600) return std::format("{} min", total / 60);
		return std::format("{} h {} min", total / 3600, (total % 3600) / 60);
	}

	// `text` cut to `limit` bytes on a code point boundary, with "..." when something was left out.
	std::string ellipsized(const std::string& text, std::size_t limit)
	{
		if (text.size() <= limit) return text;
		std::size_t cut = limit;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
		return text.substr(0, cut) + "...";
	}

	std::string trimmed(std::string text)
	{
		const auto not_space = [](unsigned char c) { return !std::isspace(c); };
		text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
		text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
		return text;
	}
}

std::vector<std::string> edt::split_pattern_list(const std::string& text)
{
	std::vector<std::string> out;
	std::size_t from = 0;
	while (from <= text.size()) {
		const std::size_t comma = text.find(',', from);
		const std::size_t end = comma == std::string::npos ? text.size() : comma;
		if (std::string piece = trimmed(text.substr(from, end - from)); !piece.empty()) out.push_back(std::move(piece));
		if (comma == std::string::npos) break;
		from = comma + 1;
	}
	return out;
}

std::string edt::join_pattern_list(const std::vector<std::string>& list)
{
	std::string out;
	for (const std::string& item : list) {
		if (!out.empty()) out += ", ";
		out += item;
	}
	return out;
}

edt::agents_panel::agents_panel(agent_policy_model& model, agents_panel_services services)
	: panel_base(k_id, k_title)
	, m_model(model)
	, m_services(std::move(services))
{
}

void edt::agents_panel::on_before_begin()
{
	ImGui::SetNextWindowSize(ImVec2(700.f, 640.f), ImGuiCond_FirstUseEver);
	if (m_focus) {
		ImGui::SetNextWindowFocus();
		m_focus = false;
	}
}

void edt::agents_panel::on_render()
{
	// The only refresh of the frame: the banner, the rows and the copy an approval refers to all come from this state.
	m_model.refresh();
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
		m_model.note_interaction();
	}
	draw_banner();
	draw_server();
	draw_sessions();
	draw_policy();
	draw_footer();
}

void edt::agents_panel::draw_banner()
{
	const bool pending = m_model.pending();
	// The copy is taken here, right before it is drawn: what Approve approves is what is on screen.
	if (pending) {
		m_model.mark_displayed();
	}
	else {
		m_model.forget_displayed();
	}
	// A fixed height whether or not anything waits, so the widgets below never shift under the mouse when it appears.
	if (ImGui::BeginChild("##approval_banner", ImVec2(0.f, ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		if (pending) {
			ImGui::PushStyleColor(ImGuiCol_Text, k_warning_color);
			ImGui::TextUnformatted("The project's agent policy lets agents do more than you approved.");
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if (ImGui::Button("Review...")) ImGui::OpenPopup(k_review_popup);
			draw_review_popup();
		}
	}
	ImGui::EndChild();
}

void edt::agents_panel::draw_review_popup()
{
	if (!ImGui::BeginPopup(k_review_popup)) return;
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
	ImGui::TextUnformatted("agent_policy.json lets agents do more than you approved. Until you approve it, the approved version applies "
		"(whatever is stricter in the file already does). Saving your own edits in this window drops these changes from the file.");
	ImGui::PopTextWrapPos();
	for (const policy_loosening& change : m_model.displayed_loosenings()) {
		// The text of a file: shortened, the whole of it in the tooltip, never a format string.
		const std::string line = std::format("{}: {} -> {}", change.what, cmd::to_string(change.from), cmd::to_string(change.to));
		ImGui::Bullet();
		ImGui::SameLine();
		ImGui::TextUnformatted(ellipsized(line, k_banner_line_bytes).c_str());
		if (line.size() > k_banner_line_bytes && ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
			ImGui::TextUnformatted(line.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}
	ImGui::Separator();
	const bool armed = m_model.approve_armed();
	ImGui::BeginDisabled(!armed);
	if (ImGui::Button("Approve this version")) {
		if (const std::optional<std::string> error = m_model.approve()) {
			m_message = *error;
			m_message_is_error = true;
		}
		else {
			m_message = "Approved.";
			m_message_is_error = false;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
	if (!armed) {
		ImGui::SameLine();
		ImGui::TextDisabled("available a moment after these changes appeared");
	}
	ImGui::EndPopup();
}

void edt::agents_panel::draw_server()
{
	if (!ImGui::CollapsingHeader("Server", ImGuiTreeNodeFlags_DefaultOpen)) return;
	agent_server* server = m_services.server ? m_services.server() : nullptr;
	if (server == nullptr) {
		ImGui::TextDisabled("There is no agent server in this editor.");
		return;
	}
	if (!m_port_loaded) {
		m_port_edit = m_services.chosen_port ? m_services.chosen_port() : 0;
		m_port_loaded = true;
	}
	bool on = server->running();
	if (ImGui::Checkbox("Accept agent connections (HTTP, 127.0.0.1)", &on) && m_services.set_server) {
		const bool took = m_services.set_server(on, static_cast<std::uint16_t>(std::clamp(m_port_edit, 0, 65535)));
		m_message = took ? (on ? "The agent server is on." : "The agent server is off; its sessions ended.")
			: "The agent server did not start (is the port taken?).";
		m_message_is_error = !took;
	}
	ImGui::SetNextItemWidth(120.f);
	ImGui::InputInt("Port (0 = any free port)", &m_port_edit, 0, 0);
	m_port_edit = std::clamp(m_port_edit, 0, 65535);
	if (on) {
		ImGui::SameLine();
		if (ImGui::Button("Restart on this port") && m_services.set_server) {
			const bool took = m_services.set_server(true, static_cast<std::uint16_t>(m_port_edit));
			m_message = took ? "The agent server runs on the new port with a new token." : "The port could not be used; the server stays as it was.";
			m_message_is_error = !took;
		}
		const std::string url = std::format("http://127.0.0.1:{}/mcp", server->port());
		ImGui::TextUnformatted("Address:");
		ImGui::SameLine();
		ImGui::TextUnformatted(url.c_str());
		ImGui::TextDisabled("The token for the client is in .snk/mcp.json of the project folder.");
	}
	else {
		ImGui::TextDisabled("Off: no agent can connect over HTTP.");
	}
}

void edt::agents_panel::draw_sessions()
{
	if (!ImGui::CollapsingHeader("Connected agents", ImGuiTreeNodeFlags_DefaultOpen)) return;
	agent_server* server = m_services.server ? m_services.server() : nullptr;
	if (server == nullptr) return;
	const std::vector<agent_session_info> sessions = server->sessions();
	if (sessions.empty()) {
		ImGui::TextDisabled("None.");
		return;
	}
	if (ImGui::BeginTable("agent_sessions", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Client");
		ImGui::TableSetupColumn("Session");
		ImGui::TableSetupColumn("Connected");
		ImGui::TableSetupColumn("Idle");
		ImGui::TableSetupColumn("");
		ImGui::TableHeadersRow();
		for (const agent_session_info& session : sessions) {
			ImGui::PushID(session.id.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			// What the client calls itself: shown as it is, never as a format string.
			ImGui::TextUnformatted(session.client_name.empty() ? "(unnamed)" : session.client_name.c_str());
			ImGui::TableNextColumn();
			const std::string shortened = session.id.substr(0, 8);
			ImGui::TextUnformatted(shortened.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(seconds_text(session.age).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(session.listening ? "listening" : seconds_text(session.idle).c_str());
			ImGui::TableNextColumn();
			if (ImGui::SmallButton("Disconnect")) server->close_session(session.id);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

void edt::agents_panel::draw_policy()
{
	if (!ImGui::CollapsingHeader("Policy", ImGuiTreeNodeFlags_DefaultOpen)) return;
	int scope_index = m_scope == policy_scope::project ? 0 : 1;
	ImGui::RadioButton("Project (agent_policy.json, committed)", &scope_index, 0);
	ImGui::SameLine();
	ImGui::RadioButton("Mine (only tightens)", &scope_index, 1);
	m_scope = scope_index == 0 ? policy_scope::project : policy_scope::personal;

	if (const std::string& error = m_model.error_of(m_scope); !error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, k_error_color);
		ImGui::TextWrapped("This file cannot be used and everything is denied until it is fixed: %s", error.c_str());
		ImGui::PopStyleColor();
	}
	if (m_model.base_changed(m_scope)) {
		ImGui::TextColored(k_warning_color, "The file changed on disk while you were editing. Revert to read it again.");
	}

	if (m_scope == policy_scope::project) {
		bool confirm = m_model.confirms_file_changes();
		if (ImGui::Checkbox("Ask me before an agent creates, deletes or renames code files", &confirm)) m_model.set_confirms_file_changes(confirm);
	}

	if (ImGui::BeginTable("agent_categories", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Category");
		ImGui::TableSetupColumn("Mode");
		ImGui::TableSetupColumn("Agents get");
		ImGui::TableSetupColumn("Because");
		ImGui::TableHeadersRow();
		for (policy_category_row& row : m_model.category_rows(m_scope)) {
			ImGui::PushID(label_of(row.category));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label_of(row.category));
			ImGui::TableNextColumn();
			std::optional<cmd::policy_mode> entry = row.entry;
			if (mode_combo("##mode", entry, row.loosest)) m_model.set_category(m_scope, row.category, entry);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(cmd::to_string(row.effective.mode));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(to_string(row.effective.source));
			if (ImGui::IsItemHovered() && !row.effective.reason.empty()) ImGui::SetTooltip("%s", row.effective.reason.c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (ImGui::TreeNode("Single commands")) {
		const char* current = label_of(m_command_category);
		ImGui::SetNextItemWidth(160.f);
		if (ImGui::BeginCombo("Category", current)) {
			for (const cmd::agent_category category : settable_categories()) {
				if (ImGui::Selectable(label_of(category), category == m_command_category)) m_command_category = category;
			}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("A command's own entry beats its category's and is the only way to lift a command's risk floor.");
		if (ImGui::BeginTable("agent_commands", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Command");
			ImGui::TableSetupColumn("Mode");
			ImGui::TableSetupColumn("Agents get");
			ImGui::TableHeadersRow();
			for (policy_command_row& row : m_model.command_rows(m_scope, m_command_category)) {
				ImGui::PushID(row.id.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.id.c_str());
				if (ImGui::IsItemHovered() && !row.summary.empty()) ImGui::SetTooltip("%s", row.summary.c_str());
				ImGui::TableNextColumn();
				std::optional<cmd::policy_mode> entry = row.entry;
				if (mode_combo("##mode", entry, cmd::policy_mode::allow)) m_model.set_command(m_scope, row.id, entry);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cmd::to_string(row.effective.mode));
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Git")) {
		draw_vcs(m_scope);
		ImGui::TreePop();
	}
}

void edt::agents_panel::draw_vcs(policy_scope scope)
{
	if (m_list_scope != scope) {
		m_list_scope = scope;
		for (bool& loaded : m_list_loaded) loaded = false;
	}
	const policy_layer& shown = m_model.layer(scope);
	const vcs_policy_layer none{};
	const vcs_policy_layer& vcs = shown.vcs ? *shown.vcs : none;
	ImGui::TextDisabled("Comma-separated patterns; empty = the default; 'none' = no entry at all. A personal list only narrows the project's.");
	struct list_def { const char* label; vcs_list_key key; const std::optional<std::vector<std::string>>* value; };
	const list_def lists[] = {
		{ "Push to branches", vcs_list_key::push_branches, &vcs.push_branches },
		{ "Remotes", vcs_list_key::remotes, &vcs.remotes },
		{ "Also protected branches", vcs_list_key::protected_branches, &vcs.protected_branches },
	};
	int index = 0;
	for (const list_def& list : lists) {
		std::string& text = m_list_text[index];
		if (!m_list_loaded[index]) {
			text = list.value->has_value() ? ((*list.value)->empty() ? std::string("none") : join_pattern_list(**list.value)) : std::string{};
			m_list_loaded[index] = true;
		}
		ImGui::SetNextItemWidth(360.f);
		if (ImGui::InputTextWithHint(list.label, "(default)", &text)) {
			const std::string cleaned = trimmed(text);
			if (cleaned.empty()) {
				m_model.set_vcs_list(scope, list.key, std::nullopt);
			}
			else if (cleaned == "none") {
				m_model.set_vcs_list(scope, list.key, std::vector<std::string>{});
			}
			else {
				m_model.set_vcs_list(scope, list.key, split_pattern_list(cleaned));
			}
		}
		// Back to what the layer holds when the field is left (a reverted edit, another scope).
		if (!ImGui::IsItemActive()) m_list_loaded[index] = false;
		++index;
	}
	struct flag_def { const char* label; vcs_flag_key key; const std::optional<bool>* value; };
	const flag_def flags[] = {
		{ "Run the repository's hooks", vcs_flag_key::allow_hooks, &vcs.allow_hooks },
		{ "Run clean/smudge filters (LFS)", vcs_flag_key::allow_filters, &vcs.allow_filters },
		{ "Use a credential helper the repository names", vcs_flag_key::allow_repo_credential_helper, &vcs.allow_repo_credential_helper },
		{ "Skip the repository config allowlist", vcs_flag_key::allow_any_repo_config, &vcs.allow_any_repo_config },
	};
	for (const flag_def& flag : flags) {
		ImGui::PushID(flag.label);
		const char* shown_value = !flag.value->has_value() ? "default (off)" : (**flag.value ? "on" : "off");
		ImGui::SetNextItemWidth(120.f);
		if (ImGui::BeginCombo(flag.label, shown_value)) {
			if (ImGui::Selectable("default (off)", !flag.value->has_value())) m_model.set_vcs_flag(scope, flag.key, std::nullopt);
			ImGui::BeginDisabled(scope == policy_scope::personal);
			if (ImGui::Selectable("on", flag.value->has_value() && **flag.value)) m_model.set_vcs_flag(scope, flag.key, true);
			ImGui::EndDisabled();
			if (ImGui::Selectable("off", flag.value->has_value() && !**flag.value)) m_model.set_vcs_flag(scope, flag.key, false);
			ImGui::EndCombo();
		}
		ImGui::PopID();
	}
}

void edt::agents_panel::draw_footer()
{
	ImGui::Separator();
	const bool dirty = m_model.dirty();
	const bool moved = m_model.base_changed(policy_scope::project) || m_model.base_changed(policy_scope::personal);
	ImGui::BeginDisabled(!dirty);
	ImGui::BeginDisabled(moved);
	if (ImGui::Button("Apply")) {
		if (const std::optional<std::string> error = m_model.apply()) {
			m_message = *error;
			m_message_is_error = true;
		}
		else {
			m_message = "Saved.";
			m_message_is_error = false;
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Revert")) {
		m_model.revert();
		m_message.clear();
		for (bool& loaded : m_list_loaded) loaded = false;
	}
	ImGui::EndDisabled();
	if (dirty && m_model.apply_drops_pending()) {
		ImGui::SameLine();
		ImGui::TextColored(k_warning_color, "Applying replaces the file with the approved version plus your edits.");
	}
	if (!m_message.empty()) ImGui::TextColored(m_message_is_error ? k_error_color : k_ok_color, "%s", m_message.c_str());
}
