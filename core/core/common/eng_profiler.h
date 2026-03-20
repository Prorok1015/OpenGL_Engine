#pragma once

#include <chrono>
#include <thread>
#include <array>
#include <vector>
#include <cstdint>

namespace ds
{
	static constexpr uint32_t PROFILER_RING_BUFFER_SIZE = 1024;

	// Timeline IDs — each represents a separate execution context
	enum class profiler_timeline : uint8_t
	{
		cpu = 0,
		gpu = 1,
		// Future: audio, job_system, etc.
	};

	struct profile_entry
	{
		const char* name = nullptr;
		float duration_us = 0.0f;
		uint8_t depth = 0;
		profiler_timeline timeline = profiler_timeline::cpu;
		uint64_t frame_number = 0;
		std::thread::id thread_id{};
	};

	struct profiler_thread_data
	{
		std::array<profile_entry, PROFILER_RING_BUFFER_SIZE> entries{};
		uint32_t write_index = 0;
		uint8_t current_depth = 0;
		uint64_t frame_number = 0;

		void push_entry(const char* name, float duration_us, profiler_timeline tl = profiler_timeline::cpu)
		{
			auto& e = entries[write_index % PROFILER_RING_BUFFER_SIZE];
			e.name = name;
			e.duration_us = duration_us;
			e.depth = current_depth;
			e.timeline = tl;
			e.frame_number = frame_number;
			e.thread_id = std::this_thread::get_id();
			++write_index;
		}

		uint32_t entry_count() const
		{
			return write_index < PROFILER_RING_BUFFER_SIZE
				? write_index
				: PROFILER_RING_BUFFER_SIZE;
		}
	};

	// Thread data access (lazy-registers thread on first call)
	profiler_thread_data& profiler_get_thread_data();

	// Mark frame boundary — increments frame counter, records prev frame duration
	void profiler_mark_frame(const char* name);

	// Copy all entries from all registered threads
	std::vector<profile_entry> profiler_snapshot();

	// Get entries for a specific frame number from all threads
	std::vector<profile_entry> profiler_snapshot_frame(uint64_t frame_number);

	// Get current frame number on calling thread
	uint64_t profiler_current_frame();

	// Aggregate stats for a single zone across a frame
	struct zone_stats
	{
		const char* name = nullptr;
		float total_us = 0.0f;
		float max_us = 0.0f;
		uint32_t call_count = 0;
		profiler_timeline timeline = profiler_timeline::cpu;

		float avg_us() const { return call_count > 0 ? total_us / call_count : 0.0f; }
	};

	// Get raw entries for the last completed frame (preserves depth and order)
	std::vector<profile_entry> profiler_last_frame_entries();

	// Get per-zone aggregate stats for the last completed frame
	std::vector<zone_stats> profiler_last_frame_stats();

	// Dump last frame stats to engine log (category "profiler")
	void profiler_dump_to_log();

	class scoped_profiler_zone
	{
	public:
		explicit scoped_profiler_zone(const char* name)
			: name_(name)
			, start_(std::chrono::high_resolution_clock::now())
		{
			auto& td = profiler_get_thread_data();
			depth_ = td.current_depth;
			++td.current_depth;
		}

		~scoped_profiler_zone()
		{
			auto end = std::chrono::high_resolution_clock::now();
			float us = std::chrono::duration<float, std::micro>(end - start_).count();

			auto& td = profiler_get_thread_data();
			--td.current_depth;
			td.push_entry(name_, us);
		}

		scoped_profiler_zone(const scoped_profiler_zone&) = delete;
		scoped_profiler_zone& operator=(const scoped_profiler_zone&) = delete;

	private:
		const char* name_;
		uint8_t depth_;
		std::chrono::high_resolution_clock::time_point start_;
	};

} // namespace ds

// Macro helpers for unique variable names
#define PROFILE_CONCAT_IMPL(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_IMPL(a, b)

// Unified check — use ENGINE_PROFILE_ENABLED in client code instead of
// checking ENGINE_PROFILE_INTERNAL || ENGINE_PROFILE_TRACY everywhere
#if defined(ENGINE_PROFILE_INTERNAL) || defined(ENGINE_PROFILE_TRACY)
#define ENGINE_PROFILE_ENABLED 1
#endif

#if defined(ENGINE_PROFILE_ENABLED)

#define PROFILE_SCOPE(name) \
	ds::scoped_profiler_zone PROFILE_CONCAT(_profiler_zone_, __LINE__)(name)

#define PROFILE_FUNCTION() \
	PROFILE_SCOPE(__FUNCTION__)

#define PROFILE_FRAME(name) \
	ds::profiler_mark_frame(name)

#else

#define PROFILE_SCOPE(name) (void)0
#define PROFILE_FUNCTION() (void)0
#define PROFILE_FRAME(name) (void)0

#endif

// Default no-op for GPU profiling — overridden by rnd_gpu_profiler.h when included
#if !defined(PROFILE_GPU_SCOPE)
#define PROFILE_GPU_SCOPE(name) (void)0
#endif
