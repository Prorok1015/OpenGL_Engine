#include "eng_profiler.h"
#include "engine_log.h"

#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <string>

namespace ds
{
	namespace
	{
		std::mutex g_registry_mutex;
		std::vector<profiler_thread_data*> g_thread_registry;

		void register_thread_data(profiler_thread_data* td)
		{
			std::lock_guard lock(g_registry_mutex);
			g_thread_registry.push_back(td);
		}
	} // anonymous namespace

	profiler_thread_data& profiler_get_thread_data()
	{
		thread_local profiler_thread_data tls_data;
		thread_local bool registered = false;
		if (!registered) {
			register_thread_data(&tls_data);
			registered = true;
		}
		return tls_data;
	}

	void profiler_mark_frame(const char* /*name*/)
	{
		auto& td = profiler_get_thread_data();

		// Frame boundary management only — no entry push.
		// Frame timing should be captured via PROFILE_SCOPE at the call site.
		++td.frame_number;
		td.current_depth = 0;
	}

	std::vector<profile_entry> profiler_snapshot()
	{
		std::lock_guard lock(g_registry_mutex);
		std::vector<profile_entry> result;

		for (auto* td : g_thread_registry) {
			uint32_t count = td->entry_count();
			uint32_t start = td->write_index >= PROFILER_RING_BUFFER_SIZE
				? td->write_index % PROFILER_RING_BUFFER_SIZE
				: 0;

			for (uint32_t i = 0; i < count; ++i) {
				result.push_back(td->entries[(start + i) % PROFILER_RING_BUFFER_SIZE]);
			}
		}

		return result;
	}

	std::vector<profile_entry> profiler_snapshot_frame(uint64_t frame_number)
	{
		std::lock_guard lock(g_registry_mutex);
		std::vector<profile_entry> result;

		for (auto* td : g_thread_registry) {
			uint32_t count = td->entry_count();
			uint32_t start = td->write_index >= PROFILER_RING_BUFFER_SIZE
				? td->write_index % PROFILER_RING_BUFFER_SIZE
				: 0;

			for (uint32_t i = 0; i < count; ++i) {
				auto& e = td->entries[(start + i) % PROFILER_RING_BUFFER_SIZE];
				if (e.frame_number == frame_number) {
					result.push_back(e);
				}
			}
		}

		return result;
	}

	uint64_t profiler_current_frame()
	{
		return profiler_get_thread_data().frame_number;
	}

	std::vector<profile_entry> profiler_last_frame_entries()
	{
		uint64_t frame = profiler_current_frame();
		if (frame == 0) return {};
		return profiler_snapshot_frame(frame - 1);
	}

	std::vector<zone_stats> profiler_last_frame_stats()
	{
		uint64_t frame = profiler_current_frame();
		if (frame == 0) return {};

		// Get entries from the previous completed frame
		auto entries = profiler_snapshot_frame(frame - 1);

		std::unordered_map<const char*, zone_stats> stats_map;
		for (auto& e : entries) {
			if (!e.name) continue;
			auto& s = stats_map[e.name];
			s.name = e.name;
			s.total_us += e.duration_us;
			s.max_us = std::max(s.max_us, e.duration_us);
			s.timeline = e.timeline;
			++s.call_count;
		}

		std::vector<zone_stats> result;
		result.reserve(stats_map.size());
		for (auto& [_, s] : stats_map) {
			result.push_back(s);
		}

		// Sort by total time descending
		std::sort(result.begin(), result.end(),
			[](const zone_stats& a, const zone_stats& b) { return a.total_us > b.total_us; });

		return result;
	}

	void profiler_dump_to_log()
	{
		auto stats = profiler_last_frame_stats();
		if (stats.empty()) {
			egLOG_INFO("profiler", "No profiler data for last frame");
			return;
		}

		uint64_t frame = profiler_current_frame();
		egLOG_INFO("profiler", "=== Profiler dump (frame {}) ===", frame - 1);
		egLOG_INFO("profiler", "{:<30s} {:>10s} {:>10s} {:>10s} {:>6s}",
			"Zone", "Total(us)", "Avg(us)", "Max(us)", "Calls");

		for (auto& s : stats) {
			egLOG_INFO("profiler", "{:<30s} {:>10.1f} {:>10.1f} {:>10.1f} {:>6d}",
				s.name, s.total_us, s.avg_us(), s.max_us, s.call_count);
		}

		egLOG_INFO("profiler", "=== End profiler dump ===");
	}

} // namespace ds
