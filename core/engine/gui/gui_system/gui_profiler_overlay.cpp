#include "gui_profiler_overlay.h"
#include "imgui.h"
#include "eng_profiler.h"
#include "engine_log.h"
#include <algorithm>
#include <cstring>
#include <format>

namespace gui
{

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

	auto raw_stats = ds::profiler_last_frame_stats();
	if (raw_stats.empty()) return;

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

	// Apply the user's current sort, not a hardcoded one
	apply_sort();

	// Reset selection (indices shifted after re-sort)
	selected_.assign(display_stats_.size(), false);
	last_clicked_row_ = -1;
#endif
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
	return std::format("{:<30s}\t{:>10.1f}\t{:>10.1f}\t{:>10.1f}\t{:>6d}",
		s.name ? s.name : "???", s.total_us, s.avg_us, s.max_us, s.call_count);
}

void profiler_overlay::copy_table_to_clipboard()
{
	std::string text;
	text += std::format("{:<30s}\t{:>10s}\t{:>10s}\t{:>10s}\t{:>6s}\n",
		"Zone", "Total(us)", "Avg(us)", "Max(us)", "Calls");

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

bool profiler_overlay::render()
{
	bool is_open = true;

#if defined(ENGINE_PROFILE_ENABLED)
	if (!pause_) {
		refresh_stats();
	}

	ImGui::SetNextWindowSize(ImVec2{560, 450}, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Profiler", &is_open)) {
		// Controls row 1
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

		ImGui::Separator();

		render_frame_bar();

		// Copy buttons — right before the table
		if (ImGui::SmallButton("[=] Copy All")) {
			copy_table_to_clipboard();
		}
		ImGui::SameLine();

		// Count selected
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

		ImGui::TextDisabled("(%zu zones | Ctrl/Shift+click to multi-select)", display_stats_.size());

		render_stats_table();
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
		ImGui::TableSetupColumn("Total (us)", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
		ImGui::TableSetupColumn("Avg (us)", ImGuiTableColumnFlags_PreferSortDescending);
		ImGui::TableSetupColumn("Max (us)", ImGuiTableColumnFlags_PreferSortDescending);
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
					// Shift+click: select range
					int from = std::min(last_clicked_row_, static_cast<int>(row));
					int to = std::max(last_clicked_row_, static_cast<int>(row));
					if (!ctrl) {
						std::fill(selected_.begin(), selected_.end(), false);
					}
					for (int i = from; i <= to; ++i) {
						selected_[i] = true;
					}
				} else if (ctrl) {
					// Ctrl+click: toggle single
					selected_[row] = !selected_[row];
				} else {
					// Plain click: select only this row
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

				// Count selected for context menu
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
			ImGui::Text("%.1f", s.total_us);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.1f", s.avg_us);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.1f", s.max_us);
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%u", s.call_count);
		}

		ImGui::EndTable();
	}
}

void profiler_overlay::render_frame_bar()
{
	if (display_stats_.empty()) return;

	float total_us = 0.0f;
	for (auto& s : display_stats_) {
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
	for (size_t i = 0; i < display_stats_.size() && i < 20; ++i) {
		float w = (display_stats_[i].total_us / total_us) * bar_width;
		if (w < 1.0f) continue;

		ImU32 color = palette[i % palette_size];
		draw->AddRectFilled(ImVec2{x, pos.y}, ImVec2{x + w, pos.y + bar_height}, color);

		if (w > 40.0f) {
			const char* name = display_stats_[i].name ? display_stats_[i].name : "?";
			draw->AddText(ImVec2{x + 2.0f, pos.y + 4.0f}, IM_COL32(255, 255, 255, 255), name);
		}

		x += w;
	}

	ImGui::Dummy(ImVec2{bar_width, bar_height});
	ImGui::Text("Frame: %.2f ms", total_us / 1000.0f);
}

} // namespace gui
