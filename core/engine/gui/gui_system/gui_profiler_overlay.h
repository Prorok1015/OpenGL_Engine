#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include "eng_profiler.h"

namespace gui
{
	class profiler_overlay
	{
	public:
		// Render the profiler window. Returns false if user closed it.
		bool render();

		// Check if F7 was pressed and dump to log
		void check_hotkey();

	private:
		// Smoothed zone data for display
		struct smoothed_zone
		{
			const char* name = nullptr;
			float total_us = 0.0f;
			float avg_us = 0.0f;
			float max_us = 0.0f;
			uint32_t call_count = 0;
		};

		std::vector<smoothed_zone> display_stats_;
		std::unordered_map<const char*, smoothed_zone> smoothed_map_;

		std::chrono::steady_clock::time_point last_update_time_{};
		bool first_update_ = true;
		bool pause_ = false;
		float update_interval_ms_ = 250.0f;
		float smoothing_ = 0.3f;

		// Remembered sort state
		int sort_column_ = 1;         // default: Total (us)
		bool sort_ascending_ = false; // default: descending

		// Multi-select state
		std::vector<bool> selected_;
		int last_clicked_row_ = -1;

		void refresh_stats();
		void apply_sort();
		void render_stats_table();
		void render_frame_bar();
		void copy_table_to_clipboard();
		void copy_selected_to_clipboard();
		std::string format_row(const smoothed_zone& s) const;
	};
} // namespace gui
