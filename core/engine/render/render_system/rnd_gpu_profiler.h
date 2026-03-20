#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include "eng_profiler.h"

namespace rnd::driver { class driver_interface; }

namespace rnd
{
	struct gpu_zone_result
	{
		const char* name = nullptr;
		float duration_us = 0.0f;
		uint64_t frame_number = 0;
	};

	class gpu_profiler
	{
	public:
		static constexpr uint32_t MAX_ZONES_PER_FRAME = 32;
		static constexpr uint32_t FRAME_LATENCY = 3;

		void init(driver::driver_interface& drv);
		void shutdown();

		void begin_frame(uint64_t frame_number);
		void end_frame();

		void begin_zone(const char* name);
		void end_zone();

		bool is_available() const { return available_; }

		// Collect completed GPU results and push them into the CPU profiler ring buffer
		void collect_and_push_results();

	private:
		struct pending_zone
		{
			const char* name = nullptr;
			uint32_t begin_query = 0;
			uint32_t end_query = 0;
			uint64_t frame_number = 0;
		};

		struct frame_data
		{
			std::vector<pending_zone> zones;
			bool submitted = false;
		};

		driver::driver_interface* drv_ = nullptr;
		std::array<frame_data, FRAME_LATENCY> frames_{};
		uint32_t current_frame_slot_ = 0;
		uint64_t current_frame_number_ = 0;
		bool available_ = false;

		// Cached last known results — pushed every frame for stable overlay
		struct cached_zone
		{
			const char* stable_name = nullptr; // pointer from name_cache
			float duration_us = 0.0f;
		};
		std::vector<cached_zone> cached_results_;

		void push_to_ring_buffer(const std::vector<cached_zone>& zones);

		// Query object pool
		std::vector<uint32_t> query_pool_;
		uint32_t pool_next_ = 0;

		uint32_t acquire_query();
		void ensure_pool_capacity(uint32_t needed);
	};

	gpu_profiler& get_gpu_profiler();

	class scoped_gpu_zone
	{
	public:
		explicit scoped_gpu_zone(const char* name)
		{
			auto& p = get_gpu_profiler();
			if (p.is_available()) {
				p.begin_zone(name);
				active_ = true;
			}
		}

		~scoped_gpu_zone()
		{
			if (active_) {
				get_gpu_profiler().end_zone();
			}
		}

		scoped_gpu_zone(const scoped_gpu_zone&) = delete;
		scoped_gpu_zone& operator=(const scoped_gpu_zone&) = delete;

	private:
		bool active_ = false;
	};

} // namespace rnd

// Override the default no-op from eng_profiler.h
#undef PROFILE_GPU_SCOPE

#if defined(ENGINE_PROFILE_ENABLED)
#define PROFILE_GPU_SCOPE(name) \
	rnd::scoped_gpu_zone PROFILE_CONCAT(_gpu_zone_, __LINE__)(name)
#else
#define PROFILE_GPU_SCOPE(name) (void)0
#endif
