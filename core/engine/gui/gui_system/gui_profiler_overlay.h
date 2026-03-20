#pragma once

#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <chrono>
#include "eng_profiler.h"

namespace gui
{
	enum class profiler_view_mode : int
	{
		flat = 0,
		hierarchy = 1
	};

	enum class profiler_time_unit : int
	{
		microseconds = 0,
		milliseconds = 1,
		seconds = 2
	};

	class profiler_overlay
	{
	public:
		// Render the profiler window. Returns false if user closed it.
		bool render();

		// Check if F7 was pressed and dump to log
		void check_hotkey();

	private:
		// Smoothed zone data for flat display
		struct smoothed_zone
		{
			const char* name = nullptr;
			float total_us = 0.0f;
			float avg_us = 0.0f;
			float max_us = 0.0f;
			uint32_t call_count = 0;
			ds::profiler_timeline timeline = ds::profiler_timeline::cpu;
		};

		// Tree node for hierarchical display
		struct hierarchy_node
		{
			const char* name = nullptr;
			float duration_us = 0.0f;
			uint32_t call_count = 1;
			uint8_t depth = 0;
			std::vector<hierarchy_node> children;
		};

		// Flat view data
		std::vector<smoothed_zone> display_stats_;
		std::unordered_map<const char*, smoothed_zone> smoothed_map_;

		// Hierarchy view data — roots grouped by timeline
		std::map<uint8_t, std::vector<hierarchy_node>> hierarchy_by_timeline_;

		std::chrono::steady_clock::time_point last_update_time_{};
		bool first_update_ = true;
		bool pause_ = false;
		float update_interval_ms_ = 250.0f;
		float smoothing_ = 0.3f;

		// Remembered sort state
		int sort_column_ = 1;         // default: Total
		bool sort_ascending_ = false; // default: descending

		// Multi-select state
		std::vector<bool> selected_;
		int last_clicked_row_ = -1;

		// View options
		profiler_view_mode view_mode_ = profiler_view_mode::flat;
		profiler_time_unit time_unit_ = profiler_time_unit::milliseconds;
		int hierarchy_timeline_filter_ = -1; // -1 = All, 0+ = specific timeline

		void refresh_stats();
		void build_hierarchy();
		void apply_sort();
		void render_controls();
		void render_stats_table();
		void render_hierarchy_view();
		void render_hierarchy_table(const std::vector<const hierarchy_node*>& roots);
		void render_hierarchy_node(const hierarchy_node& node);
		void render_frame_bar();
		void copy_table_to_clipboard();
		void copy_selected_to_clipboard();

		float convert_time(float us) const;
		const char* time_unit_suffix() const;
		std::string format_time(float us) const;
		std::string format_row(const smoothed_zone& s) const;

		static void merge_duplicate_siblings(std::vector<hierarchy_node>& children);
	};
} // namespace gui
