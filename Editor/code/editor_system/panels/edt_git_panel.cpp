#include "edt_git_panel.h"
#include "edt_history_view.h"
#include <snk/editor/edt_dialog_service.h>
#include <snk/project/prj_code.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <optional>
#include <span>

namespace {
	const ImVec4 k_added{ 0.45f, 0.80f, 0.45f, 1.0f };
	const ImVec4 k_removed{ 0.90f, 0.45f, 0.45f, 1.0f };
	const ImVec4 k_hunk{ 0.45f, 0.70f, 0.95f, 1.0f };
	const ImVec4 k_meta{ 0.65f, 0.65f, 0.65f, 1.0f };
	const ImVec4 k_error{ 1.0f, 0.45f, 0.40f, 1.0f };

	const ImVec4 k_head_ref{ 0.95f, 0.75f, 0.30f, 1.0f };
	const ImVec4 k_branch_ref{ 0.45f, 0.80f, 0.45f, 1.0f };
	const ImVec4 k_remote_ref{ 0.45f, 0.70f, 0.95f, 1.0f };
	const ImVec4 k_tag_ref{ 0.85f, 0.60f, 0.90f, 1.0f };

	const ImU32 k_lane_colours[] = { IM_COL32(90, 160, 240, 255), IM_COL32(110, 200, 110, 255), IM_COL32(240, 170, 70, 255),
		IM_COL32(220, 100, 200, 255), IM_COL32(80, 200, 200, 255), IM_COL32(230, 90, 90, 255), IM_COL32(170, 140, 240, 255),
		IM_COL32(200, 200, 90, 255) };

	ImU32 lane_colour(std::uint32_t lane)
	{
		return k_lane_colours[lane % std::size(k_lane_colours)];
	}

	const ImVec4& ref_colour(edt::vcs::history_ref::kind kind)
	{
		using ref_kind = edt::vcs::history_ref::kind;
		switch (kind) {
		case ref_kind::head: return k_head_ref;
		case ref_kind::remote: return k_remote_ref;
		case ref_kind::tag: return k_tag_ref;
		case ref_kind::branch: break;
		}
		return k_branch_ref;
	}

	std::string change_label(const edt::git_change& change, char code)
	{
		if (!change.orig_path.empty()) return std::format("{} {} <- {}", code, change.path, change.orig_path);
		return std::format("{} {}", code, change.path);
	}

	// At most k_shown_refs labels of at most k_ref_chars bytes each, then "+N": a commit many refs point at keeps its
	// subject in view.
	void draw_ref_labels(const std::vector<edt::vcs::history_ref>& refs)
	{
		constexpr std::size_t k_shown_refs = 3;
		constexpr std::size_t k_ref_chars = 24;
		for (std::size_t i = 0; i < refs.size() && i < k_shown_refs; ++i) {
			const std::string& name = refs[i].name;
			if (name.size() > k_ref_chars) {
				std::size_t cut = k_ref_chars;
				// Never inside a UTF-8 sequence.
				while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0u) == 0x80u) --cut;
				ImGui::TextColored(ref_colour(refs[i].type), "%.*s...", static_cast<int>(cut), name.c_str());
			} else {
				ImGui::TextColored(ref_colour(refs[i].type), "%s", name.c_str());
			}
			if (name.size() > k_ref_chars && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
			ImGui::SameLine();
		}
		if (refs.size() > k_shown_refs) {
			ImGui::TextDisabled("+%zu", refs.size() - k_shown_refs);
			ImGui::SameLine();
		}
	}

	void draw_diff_lines(const char* id, const std::vector<edt::git_diff_line>& lines)
	{
		using edt::git_diff_line;
		if (ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
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
}

edt::git_panel::git_panel(editor_events& events, git_panel_model::ports ports, dialog_service& dialogs, res::resource_system& resources)
	: panel_base("git", "Git"), m_model(events, ports), m_history(events, history_ports_of(ports)), m_dialogs(dialogs),
	  m_resources(resources)
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
	m_history.set_visible(false);
}

void edt::git_panel::on_render()
{
	m_model.set_visible(true);
	m_model.tick();
	if (!m_model.available()) {
		ImGui::TextDisabled("%s", m_model.read() ? m_model.unavailable_reason().c_str() : "...");
		if (ImGui::Button("Refresh")) m_model.refresh();
		m_history.set_visible(false);
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
	bool history_shown = false;
	if (ImGui::BeginTabBar("##git_tabs")) {
		if (ImGui::BeginTabItem("Changes")) {
			draw_changes();
			draw_commit();
			ImGui::Separator();
			draw_diff();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("History")) {
			history_shown = true;
			draw_history();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	if (!history_shown) m_history.set_visible(false);
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
		if (const auto& progress = m_model.busy_progress()) {
			ImGui::SameLine();
			ImGui::ProgressBar(std::clamp(progress->fraction, 0.0f, 1.0f), ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f),
				progress->text.c_str());
		}
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
	draw_diff_lines("##diff", m_model.diff());
}

void edt::git_panel::draw_history()
{
	// A commit, pull, checkout... of this panel, or one made elsewhere, moved HEAD or the refs.
	if (m_model.writes_finished() != m_seen_writes || m_model.refs_changed() != m_seen_refs) {
		m_seen_writes = m_model.writes_finished();
		m_seen_refs = m_model.refs_changed();
		m_history.reload();
	}
	m_history.set_visible(true);
	m_history.tick();
	draw_history_filter();
	if (!m_history.error().empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, k_error);
		ImGui::TextWrapped("%s", m_history.error().c_str());
		ImGui::PopStyleColor();
	}
	const float available = ImGui::GetContentRegionAvail().y;
	const bool details = !m_history.selected_commit().empty();
	draw_history_table(details ? available * 0.55f : available);
	if (details) draw_history_details();
}

void edt::git_panel::draw_history_filter()
{
	vcs::history_filter filter = m_history.filter();
	bool changed = false;
	const char* all_label = m_history.filters_by_text() ? "All branches" : "HEAD";
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	if (ImGui::BeginCombo("##branch", filter.branch.empty() ? all_label : filter.branch.c_str())) {
		if (ImGui::Selectable(all_label, filter.branch.empty())) {
			filter.branch.clear();
			changed = true;
		}
		for (const git_branch& branch : m_model.branches()) {
			ImGui::PushID(branch.name.c_str());
			if (ImGui::Selectable(branch.name.c_str(), filter.branch == branch.name)) {
				filter.branch = branch.name;
				changed = true;
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (m_history.filters_by_text()) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
		changed |= ImGui::InputTextWithHint("##author", "author", &filter.author);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		changed |= ImGui::InputTextWithHint("##text", "message text", &filter.text);
	}
	if (changed) m_history.set_filter(std::move(filter));
	ImGui::SameLine();
	if (ImGui::Button("Refresh##history")) m_history.reload();
	ImGui::SameLine();
	if (m_history.loading()) {
		ImGui::TextDisabled("%zu commits, loading...", m_history.commits().size());
	} else {
		ImGui::TextDisabled("%zu commits%s", m_history.commits().size(), m_history.at_end() ? "" : "+");
	}
}

void edt::git_panel::draw_history_table(float height)
{
	const auto& commits = m_history.commits();
	const auto& graph = m_history.graph();
	constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
		| ImGuiTableFlags_Resizable;
	if (!ImGui::BeginTable("##history", 4, flags, ImVec2(0.0f, height))) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableHeadersRow();

	const float lane_width = ImGui::GetFontSize() * 0.9f;
	// Thickness in step with the font, so a scaled UI keeps the lines visible.
	const float line_thickness = std::max(1.0f, ImGui::GetFontSize() / 13.0f * 1.5f);
	const float cell_padding = ImGui::GetStyle().CellPadding.y;
	const float line_height = ImGui::GetTextLineHeight();
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	const std::optional<std::size_t> selected = m_history.selected_row();
	bool any_shown = false;
	std::size_t first_shown = 0;
	std::size_t last_shown = 0;

	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(std::min(commits.size(), graph.rows().size())));
	while (clipper.Step()) {
		if (clipper.DisplayStart >= clipper.DisplayEnd) continue;
		const auto begin = static_cast<std::size_t>(clipper.DisplayStart);
		const auto end = static_cast<std::size_t>(clipper.DisplayEnd);
		first_shown = any_shown ? std::min(first_shown, begin) : begin;
		last_shown = any_shown ? std::max(last_shown, end - 1) : end - 1;
		any_shown = true;
		std::vector<std::uint32_t> lanes = graph.lanes_above(begin);
		for (std::size_t i = begin; i < end; ++i) {
			const vcs::history_commit& commit = commits[i];
			const vcs::graph_row& row = graph.rows()[i];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			// By hash: after a reload the same index is another commit, and an open menu must not follow it there.
			ImGui::PushID(commit.hash.c_str());
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float width = lane_width * static_cast<float>(std::max<std::uint32_t>(row.width, 1));
			if (ImGui::Selectable("##row", selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
					ImVec2(width, line_height))) {
				m_history.select(i);
			}
			if (ImGui::BeginPopupContextItem("##commit")) {
				if (ImGui::MenuItem("Copy hash")) ImGui::SetClipboardText(commit.hash.c_str());
				ImGui::EndPopup();
			}
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const float top = origin.y - cell_padding;
			const float bottom = origin.y + line_height + cell_padding;
			const auto x_of = [&](std::uint32_t lane) { return origin.x + lane_width * (static_cast<float>(lane) + 0.5f); };
			const auto y_of = [&](vcs::graph_segment::at height) {
				switch (height) {
				case vcs::graph_segment::at::top: return top;
				case vcs::graph_segment::at::middle: break;
				case vcs::graph_segment::at::bottom: return bottom;
				}
				return (top + bottom) * 0.5f;
			};
			for (const vcs::graph_segment& segment : vcs::row_segments(graph, row, lanes)) {
				draw->AddLine(ImVec2(x_of(segment.from_lane), y_of(segment.from)), ImVec2(x_of(segment.to_lane), y_of(segment.to)),
					lane_colour(segment.colour_lane), line_thickness);
			}
			draw->AddCircleFilled(ImVec2(x_of(row.lane), (top + bottom) * 0.5f), lane_width * 0.22f, lane_colour(row.lane));
			graph.advance(lanes, row);

			ImGui::TableSetColumnIndex(1);
			draw_ref_labels(commit.refs);
			ImGui::TextUnformatted(commit.subject.c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(commit.author.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(vcs::age_text(now, commit.time).c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
	if (any_shown) m_history.show_rows(first_shown, last_shown);
}

void edt::git_panel::draw_history_details()
{
	ImGui::Separator();
	const std::string& hash = m_history.selected_commit();
	ImGui::TextUnformatted(hash.c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(hash.c_str());
	if (const auto row = m_history.selected_row()) {
		const vcs::history_commit& commit = m_history.commits()[*row];
		ImGui::TextDisabled("%s <%s>", commit.author.c_str(), commit.email.c_str());
		ImGui::TextWrapped("%s", commit.subject.c_str());
	}
	if (!m_history.filters_by_text()) return;

	const float files_width = ImGui::GetContentRegionAvail().x * 0.35f;
	if (ImGui::BeginChild("##commit_files", ImVec2(files_width, 0.0f), ImGuiChildFlags_Borders)) {
		if (m_history.files_loading()) ImGui::TextDisabled("...");
		const auto& files = m_history.files();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(files.size()));
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const vcs::history_file& file = files[static_cast<std::size_t>(i)];
				const std::string label = file.orig_path.empty() ? std::format("{} {}", file.status, file.path)
																 : std::format("{} {} <- {}", file.status, file.path, file.orig_path);
				if (ImGui::Selectable(label.c_str(), m_history.selected_file() == file.path)) m_history.select_file(file.path);
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginGroup();
	if (m_history.diff_version() != m_history_diff_version) {
		m_history_diff_version = m_history.diff_version();
		m_history_diff = diff_lines(m_history.diff_text());
	}
	if (m_history.selected_file().empty()) {
		ImGui::TextDisabled("Select a file to see what the commit changed in it.");
	} else {
		if (m_history.diff_truncated()) ImGui::TextDisabled("(cut at 256 KiB)");
		draw_diff_lines("##commit_diff", m_history_diff);
	}
	ImGui::EndGroup();
}
