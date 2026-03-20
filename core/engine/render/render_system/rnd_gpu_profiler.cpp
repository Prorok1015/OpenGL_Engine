#include "rnd_gpu_profiler.h"
#include "rnd_driver_interface.h"
#include "engine_log.h"
#include <unordered_map>
#include <memory>
#include <string>

static rnd::gpu_profiler g_gpu_profiler;

rnd::gpu_profiler& rnd::get_gpu_profiler()
{
	return g_gpu_profiler;
}

void rnd::gpu_profiler::init(driver::driver_interface& drv)
{
	drv_ = &drv;
	available_ = drv.supports_timer_queries();

	if (!available_) {
		egLOG_WARN("profiler/gpu", "GPU timer queries not supported — GPU profiling disabled");
		return;
	}

	// Pre-allocate query pool (2 queries per zone × MAX_ZONES × FRAME_LATENCY)
	uint32_t initial_count = MAX_ZONES_PER_FRAME * FRAME_LATENCY * 2;
	query_pool_.resize(initial_count);
	for (uint32_t i = 0; i < initial_count; ++i) {
		query_pool_[i] = drv_->create_timer_query();
	}
	pool_next_ = 0;

	for (auto& f : frames_) {
		f.zones.reserve(MAX_ZONES_PER_FRAME);
		f.submitted = false;
	}

	egLOG_INFO("profiler/gpu", "GPU profiler initialized with {} query objects", initial_count);
}

void rnd::gpu_profiler::shutdown()
{
	if (!drv_) return;

	for (auto id : query_pool_) {
		drv_->destroy_timer_query(id);
	}
	query_pool_.clear();

	for (auto& f : frames_) {
		f.zones.clear();
		f.submitted = false;
	}

	available_ = false;
	drv_ = nullptr;
}

void rnd::gpu_profiler::begin_frame(uint64_t frame_number)
{
	if (!available_) return;

	current_frame_number_ = frame_number;

	// Collect results from the oldest frame slot before reusing it
	collect_and_push_results();

	auto& frame = frames_[current_frame_slot_];
	frame.zones.clear();
	frame.submitted = false;
	pool_next_ = 0;
}

void rnd::gpu_profiler::end_frame()
{
	if (!available_) return;

	auto& frame = frames_[current_frame_slot_];
	frame.submitted = true;

	// If no zones were recorded this frame, clear cache immediately —
	// the passes stopped running, no point showing stale data.
	if (frame.zones.empty()) {
		cached_results_.clear();
	}

	current_frame_slot_ = (current_frame_slot_ + 1) % FRAME_LATENCY;
}

void rnd::gpu_profiler::begin_zone(const char* name)
{
	if (!available_) return;

	auto& frame = frames_[current_frame_slot_];
	if (frame.zones.size() >= MAX_ZONES_PER_FRAME) return;

	uint32_t begin_q = acquire_query();
	drv_->timestamp_query(begin_q);

	pending_zone zone;
	zone.name = name;
	zone.begin_query = begin_q;
	zone.end_query = 0;
	zone.frame_number = current_frame_number_;
	frame.zones.push_back(zone);
}

void rnd::gpu_profiler::end_zone()
{
	if (!available_) return;

	auto& frame = frames_[current_frame_slot_];
	if (frame.zones.empty()) return;

	// Find the last zone without an end query
	for (auto it = frame.zones.rbegin(); it != frame.zones.rend(); ++it) {
		if (it->end_query == 0) {
			it->end_query = acquire_query();
			drv_->timestamp_query(it->end_query);
			return;
		}
	}
}

void rnd::gpu_profiler::collect_and_push_results()
{
	if (!available_) return;

	// Persistent cache: original zone name → stable "[GPU] name" string.
	// The overlay uses const char* pointer identity as map key,
	// so we must return the SAME pointer for the same zone name every time.
	static std::unordered_map<const char*, std::unique_ptr<char[]>> name_cache;

	auto get_stable_name = [&](const char* raw_name) -> const char* {
		auto it = name_cache.find(raw_name);
		if (it == name_cache.end()) {
			char name_buf[256];
			auto formatted = std::format_to_n(name_buf, sizeof(name_buf) - 1, "[GPU] {}", raw_name);
			*formatted.out = '\0';
			auto buf = std::make_unique<char[]>(formatted.size + 1);
			std::memcpy(buf.get(), name_buf, formatted.size + 1);
			it = name_cache.emplace(raw_name, std::move(buf)).first;
		}
		return it->second.get();
	};

	// Try to collect new results from the oldest submitted frame
	uint32_t collect_slot = current_frame_slot_;
	auto& frame = frames_[collect_slot];

	if (frame.submitted && !frame.zones.empty()) {
		auto& last_zone = frame.zones.back();
		uint32_t check_query = last_zone.end_query ? last_zone.end_query : last_zone.begin_query;

		if (drv_->is_query_ready(check_query)) {
			// New results available — update cache
			cached_results_.clear();

			for (auto& zone : frame.zones) {
				if (zone.end_query == 0) continue;

				uint64_t begin_ns = drv_->get_query_result_ns(zone.begin_query);
				uint64_t end_ns = drv_->get_query_result_ns(zone.end_query);

				float duration_us = 0.0f;
				if (end_ns > begin_ns) {
					duration_us = static_cast<float>(end_ns - begin_ns) / 1000.0f;
				}

				cached_results_.push_back({ get_stable_name(zone.name), duration_us });
			}

			frame.zones.clear();
			frame.submitted = false;
		}
	}

	// Always push cached results — GPU data present every frame
	push_to_ring_buffer(cached_results_);
}

void rnd::gpu_profiler::push_to_ring_buffer(const std::vector<cached_zone>& zones)
{
	if (zones.empty()) return;

	auto& td = ds::profiler_get_thread_data();
	for (auto& z : zones) {
		td.push_entry(z.stable_name, z.duration_us, ds::profiler_timeline::gpu);
	}
}

uint32_t rnd::gpu_profiler::acquire_query()
{
	ensure_pool_capacity(pool_next_ + 1);
	return query_pool_[pool_next_++];
}

void rnd::gpu_profiler::ensure_pool_capacity(uint32_t needed)
{
	if (needed <= query_pool_.size()) return;

	uint32_t old_size = static_cast<uint32_t>(query_pool_.size());
	uint32_t new_size = std::max(needed, old_size * 2);
	query_pool_.resize(new_size);

	for (uint32_t i = old_size; i < new_size; ++i) {
		query_pool_[i] = drv_->create_timer_query();
	}
}
