#include "edt_git_panel.h"
#include <snk/editor/edt_dialog_service.h>
#include <snk/project/prj_code.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"
#include <format>
#include <span>

namespace {
	const ImVec4 k_added{ 0.45f, 0.80f, 0.45f, 1.0f };
	const ImVec4 k_removed{ 0.90f, 0.45f, 0.45f, 1.0f };
	const ImVec4 k_hunk{ 0.45f, 0.70f, 0.95f, 1.0f };
	const ImVec4 k_meta{ 0.65f, 0.65f, 0.65f, 1.0f };
	const ImVec4 k_error{ 1.0f, 0.45f, 0.40f, 1.0f };

	std::string change_label(const edt::git_change& change, char code)
	{
		if (!change.orig_path.empty()) return std::format("{} {} <- {}", code, change.path, change.orig_path);
		return std::format("{} {}", code, change.path);
	}
}

edt::git_panel::git_panel(editor_events& events, git_panel_model::ports ports, dialog_service& dialogs, res::resource_system& resources)
	: panel_base("git", "Git"), m_model(events, std::move(ports)), m_dialogs(dialogs), m_resources(resources)
{
	// Whether or not code/ is mounted yet: a project that comes later still refreshes the panel.
	m_resources.subscribe_changes(prj::k_code_protocol, this, [this](std::span<const res::file_change>) { m_model.files_changed(); });
}

edt::git_panel::~git_panel()
{
	m_resources.unsubscribe_changes(this);
}

void edt::git_panel::on_hidden()
{
	m_model.set_visible(false);
}

void edt::git_panel::on_render()
{
	m_model.set_visible(true);
	m_model.tick();
	if (!m_model.available()) {
		ImGui::TextDisabled("%s", m_model.read() ? m_model.unavailable_reason().c_str() : "...");
		if (ImGui::Button("Refresh")) m_model.refresh();
		return;
	}
	draw_header();
	if (!m_model.error().empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, k_error);
		ImGui::TextWrapped("%s", m_model.error().c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::SmallButton("Dismiss")) m_model.clear_error();
	}
	ImGui::Separator();
	draw_changes();
	draw_commit();
	ImGui::Separator();
	draw_diff();
}

void edt::git_panel::draw_header()
{
	const std::string head = m_model.detached() ? std::format("(detached) {}", m_model.head()) : m_model.head();
	ImGui::Text("Branch: %s", head.c_str());
	if (!m_model.upstream().empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("-> %s  ahead %lld, behind %lld", m_model.upstream().c_str(),
			static_cast<long long>(m_model.ahead().value_or(0)), static_cast<long long>(m_model.behind().value_or(0)));
	}

	const bool busy = !m_model.busy().empty();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button("Refresh")) m_model.refresh();
	ImGui::SameLine();
	if (ImGui::Button("Fetch")) m_model.fetch();
	ImGui::SameLine();
	if (ImGui::Button("Pull")) m_model.pull();
	ImGui::SameLine();
	if (ImGui::Button("Push")) m_model.push();
	ImGui::EndDisabled();
	if (busy) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s: running...", m_model.busy().c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Cancel")) m_model.cancel();
	} else if (m_model.loading()) {
		ImGui::SameLine();
		ImGui::TextDisabled("reading...");
	}

	ImGui::SetNextItemWidth(220.0f);
	ImGui::BeginDisabled(busy);
	if (ImGui::BeginCombo("##branches", head.c_str())) {
		for (const git_branch& branch : m_model.branches()) {
			if (ImGui::Selectable(branch.name.c_str(), branch.head) && !branch.head) {
				if (m_model.has_tracked_changes()) {
					// git refuses a switch that would lose them; this only says so before it tries.
					m_dialogs.choose("Switch branch", std::format("There are uncommitted changes. Switch to '{}' anyway?", branch.name),
						{ "Switch", "Cancel" }, [this, branch, alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> answer) {
							if (answer == 0u && !alive.expired()) m_model.checkout(branch);
						});
				} else {
					m_model.checkout(branch);
				}
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.0f);
	ImGui::InputTextWithHint("##new_branch", "new branch", &m_new_branch);
	ImGui::SameLine();
	ImGui::BeginDisabled(m_new_branch.empty());
	if (ImGui::Button("Create")) {
		m_model.create_branch(m_new_branch);
		m_new_branch.clear();
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
}

void edt::git_panel::draw_changes()
{
	const bool busy = !m_model.busy().empty();
	const auto list = [&](const char* title, const std::vector<git_change>& changes, bool staged, bool untracked) {
		const std::string header = std::format("{} ({})###{}", title, changes.size(), title);
		if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
		ImGui::PushID(title);
		for (const git_change& change : changes) {
			ImGui::PushID(change.path.c_str());
			bool checked = staged;
			ImGui::BeginDisabled(busy);
			if (ImGui::Checkbox("##stage", &checked)) m_model.set_staged(change.path, checked);
			ImGui::EndDisabled();
			ImGui::SameLine();
			const char code = untracked ? '?' : staged ? change.index : change.worktree;
			const bool selected = m_model.selected() == change.path && m_model.selected_staged() == staged;
			if (ImGui::Selectable(change_label(change, code).c_str(), selected)) m_model.select(change.path, staged);
			ImGui::PopID();
		}
		ImGui::PopID();
	};
	list("Staged", m_model.staged(), true, false);
	list("Changes", m_model.unstaged(), false, false);
	list("Untracked", m_model.untracked(), false, true);
}

void edt::git_panel::draw_commit()
{
	ImGui::InputTextMultiline("##message", &m_model.message(), ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f));
	ImGui::BeginDisabled(!m_model.can_commit());
	if (ImGui::Button("Commit")) m_model.commit();
	ImGui::EndDisabled();
	if (m_model.staged().empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("stage changes to commit them");
	}
}

void edt::git_panel::draw_diff()
{
	if (m_model.selected().empty()) {
		ImGui::TextDisabled("Select a file to see its changes.");
		return;
	}
	ImGui::Text("%s%s", m_model.selected().c_str(), m_model.selected_staged() ? " (staged)" : "");
	if (!m_model.diff_note().empty()) {
		ImGui::TextDisabled("%s", m_model.diff_note().c_str());
		return;
	}
	if (m_model.diff_truncated()) {
		ImGui::SameLine();
		ImGui::TextDisabled("(cut at 256 KiB)");
	}
	if (ImGui::BeginChild("##diff", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
		const auto& lines = m_model.diff();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(lines.size()));
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const git_diff_line& line = lines[static_cast<std::size_t>(i)];
				const ImVec4* color = nullptr;
				switch (line.type) {
				case git_diff_line::kind::added: color = &k_added; break;
				case git_diff_line::kind::removed: color = &k_removed; break;
				case git_diff_line::kind::hunk: color = &k_hunk; break;
				case git_diff_line::kind::meta:
				case git_diff_line::kind::note: color = &k_meta; break;
				case git_diff_line::kind::context: break;
				}
				if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
				ImGui::TextUnformatted(line.text.data(), line.text.data() + line.text.size());
				if (color) ImGui::PopStyleColor();
			}
		}
	}
	ImGui::EndChild();
}
