#include "gui_profiler_overlay.h"
#include "imgui.h"
#include "eng_profiler.h"
#include "engine_log.h"
#include <algorithm>
#include <cstring>
#include <format>

namespace gui
{

float profiler_overlay::convert_time(float us) const
{
	switch (time_unit_) {
	case profiler_time_unit::microseconds: return us;
	case profiler_time_unit::milliseconds: return us / 1000.0f;
	case profiler_time_unit::seconds: return us / 1000000.0f;
	}
	return us;
}

const char* profiler_overlay::time_unit_suffix() const
{
	switch (time_unit_) {
	case profiler_time_unit::microseconds: return "us";
	case profiler_time_unit::milliseconds: return "ms";
	case profiler_time_unit::seconds: return "s";
	}
	return "us";
}

std::string profiler_overlay::format_time(float us) const
{
	float val = convert_time(us);
	switch (time_unit_) {
	case profiler_time_unit::microseconds: return std::format("{:.1f}", val);
	case profiler_time_unit::milliseconds: return std::format("{:.3f}", val);
	case profiler_time_unit::seconds: return std::format("{:.6f}", val);
	}
	return std::format("{:.1f}", val);
}

void profiler_overlay::apply_sort()
{
	std::sort(display_stats_.begin(), display_stats_.end(),
		[&](const smoothed_zone& a, const smoothed_zone& b) {
			int cmp = 0;
			switch (sort_column_) {
			case 0: cmp = strcmp(a.name ? a.name : "", b.name ? b.name : ""); break;
			case 1: cmp = (a.total_us < b.total_us) ? -1 : (a.total_us > b.total_us) ? 1 : 0; break;
			case 2: cmp = (a.avg_us < b.avg_us) ? -1 : (a.avg_us > b.avg_us) ? 1 : 0; break;
			case 3: cmp = (a.max_us < b.max_us) ? -1 : (a.max_us > b.max_us) ? 1 : 0; break;
			case 4: cmp = (a.call_count < b.call_count) ? -1 : (a.call_count > b.call_count) ? 1 : 0; break;
			}
			return sort_ascending_ ? cmp < 0 : cmp > 0;
		});
}

void profiler_overlay::refresh_stats()
{
#if defined(ENGINE_PROFILE_ENABLED)
	auto now = std::chrono::steady_clock::now();

	if (first_update_) {
		last_update_time_ = now;
		first_update_ = false;
	}

	float elapsed_ms = std::chrono::duration<float, std::milli>(now - last_update_time_).count();
	if (elapsed_ms < update_interval_ms_) return;
	last_update_time_ = now;

	// Update flat stats
	auto raw_stats = ds::profiler_last_frame_stats();
	if (!raw_stats.empty()) {
		float alpha = smoothing_;

		for (auto& raw : raw_stats) {
			if (!raw.name) continue;

			auto it = smoothed_map_.find(raw.name);
			if (it == smoothed_map_.end()) {
				smoothed_zone sz;
				sz.name = raw.name;
				sz.total_us = raw.total_us;
				sz.avg_us = raw.avg_us();
				sz.max_us = raw.max_us;
				sz.call_count = raw.call_count;
				sz.timeline = raw.timeline;
				smoothed_map_[raw.name] = sz;
			} else {
				auto& sz = it->second;
				sz.total_us = alpha * raw.total_us + (1.0f - alpha) * sz.total_us;
				sz.avg_us = alpha * raw.avg_us() + (1.0f - alpha) * sz.avg_us;
				sz.max_us = alpha * raw.max_us + (1.0f - alpha) * sz.max_us;
				sz.call_count = raw.call_count;
			}
		}

		display_stats_.clear();
		display_stats_.reserve(smoothed_map_.size());
		for (auto& [_, sz] : smoothed_map_) {
			display_stats_.push_back(sz);
		}

		apply_sort();

		selected_.assign(display_stats_.size(), false);
		last_clicked_row_ = -1;
	}

	// Update hierarchy
	build_hierarchy();
#endif
}

void profiler_overlay::build_hierarchy()
{
#if defined(ENGINE_PROFILE_ENABLED)
	auto entries = ds::profiler_last_frame_entries();
	if (entries.empty()) return; // Keep previous hierarchy

	hierarchy_by_timeline_.clear();

	// Group entries by timeline — each timeline builds its own tree independently.
	std::unordered_map<uint8_t, std::vector<const ds::profile_entry*>> by_timeline;
	for (const auto& e : entries) {
		if (!e.name) continue;
		by_timeline[static_cast<uint8_t>(e.timeline)].push_back(&e);
	}

	// Build a tree per timeline
	for (auto& [tl_id, tl_entries] : by_timeline) {
		std::vector<hierarchy_node> stack;

		for (const auto* e : tl_entries) {
			hierarchy_node node;
			node.name = e->name;
			node.duration_us = e->duration_us;
			node.depth = e->depth;

			while (!stack.empty() && stack.back().depth > e->depth) {
				node.children.push_back(std::move(stack.back()));
				stack.pop_back();
			}
			std::reverse(node.children.begin(), node.children.end());

			stack.push_back(std::move(node));
		}

		auto& roots = hierarchy_by_timeline_[tl_id];
		roots = std::move(stack);

		// Merge duplicate siblings (e.g. FixedGraph executing twice per frame)
		for (auto& root : roots) {
			merge_duplicate_siblings(root.children);
		}
	}
#endif
}

void profiler_overlay::merge_duplicate_siblings(std::vector<hierarchy_node>& children)
{
	// Merge children with the same name: sum durations, combine sub-children.
	// Preserves order of first occurrence.
	std::unordered_map<const char*, size_t> name_to_index;
	std::vector<hierarchy_node> merged;

	for (auto& child : children) {
		auto it = name_to_index.find(child.name);
		if (it != name_to_index.end()) {
			auto& existing = merged[it->second];
			existing.duration_us += child.duration_us;
			existing.call_count += child.call_count;
			for (auto& gc : child.children) {
				existing.children.push_back(std::move(gc));
			}
		} else {
			name_to_index[child.name] = merged.size();
			merged.push_back(std::move(child));
		}
	}

	children = std::move(merged);

	// Recurse into each merged child
	for (auto& child : children) {
		merge_duplicate_siblings(child.children);
	}
}

void profiler_overlay::check_hotkey()
{
#if defined(ENGINE_PROFILE_ENABLED)
	if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
		ds::profiler_dump_to_log();
	}
#endif
}

std::string profiler_overlay::format_row(const smoothed_zone& s) const
{
	auto suffix = time_unit_suffix();
	return std::format("{:<30s}\t{:>12s}\t{:>12s}\t{:>12s}\t{:>6d}",
		s.name ? s.name : "???",
		format_time(s.total_us),
		format_time(s.avg_us),
		format_time(s.max_us),
		s.call_count);
}

void profiler_overlay::copy_table_to_clipboard()
{
	auto suffix = time_unit_suffix();
	std::string text;
	text += std::format("{:<30s}\t{:>12s}\t{:>12s}\t{:>12s}\t{:>6s}\n",
		"Zone",
		std::format("Total({})", suffix),
		std::format("Avg({})", suffix),
		std::format("Max({})", suffix),
		"Calls");

	for (auto& s : display_stats_) {
		text += format_row(s);
		text += '\n';
	}

	ImGui::SetClipboardText(text.c_str());
}

void profiler_overlay::copy_selected_to_clipboard()
{
	std::string text;
	for (size_t i = 0; i < display_stats_.size() && i < selected_.size(); ++i) {
		if (selected_[i]) {
			text += format_row(display_stats_[i]);
			text += '\n';
		}
	}
	if (!text.empty()) {
		ImGui::SetClipboardText(text.c_str());
	}
}

void profiler_overlay::render_controls()
{
	// Row 1: Pause, Dump, Interval, Smoothing
	ImGui::Checkbox("Pause", &pause_);
	ImGui::SameLine();
	if (ImGui::Button("Dump to Log (F7)")) {
		ds::profiler_dump_to_log();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::SliderFloat("Interval", &update_interval_ms_, 50.0f, 2000.0f, "%.0f ms");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::SliderFloat("Smoothing", &smoothing_, 0.05f, 1.0f, "%.2f");

	// Row 2: View mode, Time unit
	static const char* view_modes[] = { "Flat", "Hierarchy" };
	ImGui::SetNextItemWidth(100.0f);
	ImGui::Combo("View", reinterpret_cast<int*>(&view_mode_), view_modes, 2);

	ImGui::SameLine();
	static const char* time_units[] = { "us", "ms", "s" };
	ImGui::SetNextItemWidth(60.0f);
	ImGui::Combo("Unit", reinterpret_cast<int*>(&time_unit_), time_units, 3);

	ImGui::SameLine();
	ImGui::TextDisabled("(%zu zones)", display_stats_.size());
}

bool profiler_overlay::render()
{
	bool is_open = true;

#if defined(ENGINE_PROFILE_ENABLED)
	if (!pause_) {
		refresh_stats();
	}

	ImGui::SetNextWindowSize(ImVec2{620, 500}, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Profiler", &is_open)) {
		render_controls();

		ImGui::Separator();
		render_frame_bar();

		// Copy buttons
		if (view_mode_ == profiler_view_mode::flat) {
			if (ImGui::SmallButton("[=] Copy All")) {
				copy_table_to_clipboard();
			}
			ImGui::SameLine();

			size_t sel_count = 0;
			for (size_t i = 0; i < selected_.size() && i < display_stats_.size(); ++i) {
				if (selected_[i]) ++sel_count;
			}

			if (sel_count > 0) {
				auto label = std::format("[=] Copy {} selected", sel_count);
				if (ImGui::SmallButton(label.c_str())) {
					copy_selected_to_clipboard();
				}
				ImGui::SameLine();
			}

			ImGui::TextDisabled("(Ctrl/Shift+click to multi-select)");
		}

		// Render the selected view
		if (view_mode_ == profiler_view_mode::flat) {
			render_stats_table();
		} else {
			render_hierarchy_view();
		}
	}
	ImGui::End();
#else
	ImGui::SetNextWindowSize(ImVec2{300, 80}, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Profiler", &is_open)) {
		ImGui::TextWrapped("Profiling disabled. Build with ENGINE_PROFILE_MODE=INTERNAL");
	}
	ImGui::End();
#endif

	return is_open;
}

void profiler_overlay::render_stats_table()
{
	if (display_stats_.empty()) {
		ImGui::Text("No profiling data");
		return;
	}

	auto suffix = time_unit_suffix();

	constexpr ImGuiTableFlags flags =
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Sortable |
		ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_Resizable;

	float table_height = ImGui::GetContentRegionAvail().y;
	if (table_height < 50.0f) table_height = 200.0f;

	if (ImGui::BeginTable("ProfilerStats", 5, flags, ImVec2{0, table_height})) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_None);

		auto total_label = std::format("Total ({})", suffix);
		auto avg_label = std::format("Avg ({})", suffix);
		auto max_label = std::format("Max ({})", suffix);

		ImGui::TableSetupColumn(total_label.c_str(), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
		ImGui::TableSetupColumn(avg_label.c_str(), ImGuiTableColumnFlags_PreferSortDescending);
		ImGui::TableSetupColumn(max_label.c_str(), ImGuiTableColumnFlags_PreferSortDescending);
		ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_PreferSortDescending);
		ImGui::TableHeadersRow();

		// Read sort specs from ImGui and remember them
		if (auto* sort_specs = ImGui::TableGetSortSpecs()) {
			if (sort_specs->SpecsDirty && sort_specs->SpecsCount > 0) {
				auto& spec = sort_specs->Specs[0];
				sort_column_ = spec.ColumnIndex;
				sort_ascending_ = (spec.SortDirection == ImGuiSortDirection_Ascending);
				apply_sort();
				sort_specs->SpecsDirty = false;
			}
		}

		// Ensure selection vector matches display size
		if (selected_.size() != display_stats_.size()) {
			selected_.assign(display_stats_.size(), false);
			last_clicked_row_ = -1;
		}

		for (size_t row = 0; row < display_stats_.size(); ++row) {
			auto& s = display_stats_[row];
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(static_cast<int>(row));

			bool is_selected = selected_[row];
			if (ImGui::Selectable(s.name ? s.name : "???", is_selected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			{
				bool ctrl = ImGui::GetIO().KeyCtrl;
				bool shift = ImGui::GetIO().KeyShift;

				if (shift && last_clicked_row_ >= 0) {
					int from = std::min(last_clicked_row_, static_cast<int>(row));
					int to = std::max(last_clicked_row_, static_cast<int>(row));
					if (!ctrl) {
						std::fill(selected_.begin(), selected_.end(), false);
					}
					for (int i = from; i <= to; ++i) {
						selected_[i] = true;
					}
				} else if (ctrl) {
					selected_[row] = !selected_[row];
				} else {
					std::fill(selected_.begin(), selected_.end(), false);
					selected_[row] = true;
				}
				last_clicked_row_ = static_cast<int>(row);
			}

			// Right-click context menu
			if (ImGui::BeginPopupContextItem("RowCtx")) {
				if (ImGui::MenuItem("Copy row")) {
					ImGui::SetClipboardText((format_row(s) + "\n").c_str());
				}
				if (ImGui::MenuItem("Copy zone name")) {
					ImGui::SetClipboardText(s.name ? s.name : "");
				}

				size_t ctx_sel = 0;
				for (auto b : selected_) { if (b) ++ctx_sel; }
				if (ctx_sel > 1) {
					auto label = std::format("Copy {} selected rows", ctx_sel);
					if (ImGui::MenuItem(label.c_str())) {
						copy_selected_to_clipboard();
					}
				}
				ImGui::EndPopup();
			}

			ImGui::PopID();

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", format_time(s.total_us).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%s", format_time(s.avg_us).c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%s", format_time(s.max_us).c_str());
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%u", s.call_count);
		}

		ImGui::EndTable();
	}
}

void profiler_overlay::render_hierarchy_view()
{
	if (hierarchy_by_timeline_.empty()) {
		ImGui::Text("No profiling data");
		return;
	}

	// Timeline name lookup
	auto timeline_name = [](uint8_t id) -> const char* {
		switch (static_cast<ds::profiler_timeline>(id)) {
		case ds::profiler_timeline::cpu: return "CPU";
		case ds::profiler_timeline::gpu: return "GPU";
		}
		return "Unknown";
	};

	if (ImGui::BeginTabBar("TimelineTabs")) {
		// "All" tab — all timelines combined
		if (ImGui::BeginTabItem("All")) {
			hierarchy_timeline_filter_ = -1;
			std::vector<const hierarchy_node*> all_roots;
			for (auto& [tl_id, roots] : hierarchy_by_timeline_) {
				for (auto& r : roots) all_roots.push_back(&r);
			}
			render_hierarchy_table(all_roots);
			ImGui::EndTabItem();
		}

		// Per-timeline tabs
		for (auto& [tl_id, roots] : hierarchy_by_timeline_) {
			if (ImGui::BeginTabItem(timeline_name(tl_id))) {
				hierarchy_timeline_filter_ = static_cast<int>(tl_id);
				std::vector<const hierarchy_node*> ptrs;
				for (auto& r : roots) ptrs.push_back(&r);
				render_hierarchy_table(ptrs);
				ImGui::EndTabItem();
			}
		}

		ImGui::EndTabBar();
	}
}

void profiler_overlay::render_hierarchy_table(const std::vector<const hierarchy_node*>& roots)
{
	auto suffix = time_unit_suffix();

	constexpr ImGuiTableFlags flags =
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_Resizable;

	float table_height = ImGui::GetContentRegionAvail().y;
	if (table_height < 50.0f) table_height = 200.0f;

	auto total_label = std::format("Total ({})", suffix);
	auto avg_label = std::format("Avg ({})", suffix);

	if (ImGui::BeginTable("ProfilerHierarchy", 4, flags, ImVec2{0, table_height})) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(total_label.c_str(), ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 45.0f);
		ImGui::TableSetupColumn(avg_label.c_str(), ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableHeadersRow();

		for (const auto* root : roots) {
			render_hierarchy_node(*root);
		}

		ImGui::EndTable();
	}
}

void profiler_overlay::render_hierarchy_node(const hierarchy_node& node)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);

	// Use name pointer as stable ID — tree state survives when sibling count changes
	ImGui::PushID(node.name);

	bool has_children = !node.children.empty();
	ImGuiTreeNodeFlags tree_flags =
		ImGuiTreeNodeFlags_SpanFullWidth;

	if (!has_children) {
		tree_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	}

	bool is_open = ImGui::TreeNodeEx(node.name ? node.name : "???", tree_flags);

	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%s", format_time(node.duration_us).c_str());

	ImGui::TableSetColumnIndex(2);
	if (node.call_count > 1) {
		ImGui::Text("%u", node.call_count);
	}

	ImGui::TableSetColumnIndex(3);
	if (node.call_count > 1) {
		float avg_us = node.duration_us / static_cast<float>(node.call_count);
		ImGui::Text("%s", format_time(avg_us).c_str());
	}

	if (has_children && is_open) {
		for (const auto& child : node.children) {
			render_hierarchy_node(child);
		}
		ImGui::TreePop();
	}

	ImGui::PopID();
}

void profiler_overlay::render_frame_bar()
{
	if (display_stats_.empty()) return;

	// In hierarchy mode with a specific timeline tab, filter the bar
	bool filter = (view_mode_ == profiler_view_mode::hierarchy && hierarchy_timeline_filter_ >= 0);
	auto tl_filter = static_cast<ds::profiler_timeline>(hierarchy_timeline_filter_);

	float total_us = 0.0f;
	for (auto& s : display_stats_) {
		if (filter && s.timeline != tl_filter) continue;
		total_us += s.total_us;
	}
	if (total_us <= 0.0f) return;

	ImVec2 avail = ImGui::GetContentRegionAvail();
	float bar_width = avail.x;
	float bar_height = 24.0f;

	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImDrawList* draw = ImGui::GetWindowDrawList();

	draw->AddRectFilled(pos, ImVec2{pos.x + bar_width, pos.y + bar_height}, IM_COL32(40, 40, 40, 255));

	static constexpr ImU32 palette[] = {
		IM_COL32(66, 133, 244, 255),
		IM_COL32(219, 68, 55, 255),
		IM_COL32(244, 180, 0, 255),
		IM_COL32(15, 157, 88, 255),
		IM_COL32(171, 71, 188, 255),
		IM_COL32(255, 112, 67, 255),
		IM_COL32(0, 172, 193, 255),
		IM_COL32(124, 179, 66, 255),
	};
	constexpr int palette_size = sizeof(palette) / sizeof(palette[0]);

	float x = pos.x;
	int drawn = 0;
	for (size_t i = 0; i < display_stats_.size() && drawn < 20; ++i) {
		auto& s = display_stats_[i];
		if (filter && s.timeline != tl_filter) continue;

		float w = (s.total_us / total_us) * bar_width;
		if (w < 1.0f) { ++drawn; continue; }

		ImU32 color = palette[drawn % palette_size];
		draw->AddRectFilled(ImVec2{x, pos.y}, ImVec2{x + w, pos.y + bar_height}, color);

		if (w > 40.0f) {
			const char* name = s.name ? s.name : "?";
			draw->AddText(ImVec2{x + 2.0f, pos.y + 4.0f}, IM_COL32(255, 255, 255, 255), name);
		}

		x += w;
		++drawn;
	}

	ImGui::Dummy(ImVec2{bar_width, bar_height});
	ImGui::Text("Frame: %s %s", format_time(total_us).c_str(), time_unit_suffix());
}

} // namespace gui
