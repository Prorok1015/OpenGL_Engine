#include "mem_allocator.h"
#include <cstddef>
#include "mem_linear_allocator.h"
#include "mem_debug_resource.h"
#include "engine_log.h"

namespace {

	constexpr std::size_t default_frame_capacity = 2 * 1024 * 1024; // 2 MB

	struct frame_allocator_storage
	{
		ds::linear_resource linear;
#ifndef NDEBUG
		ds::debug_resource  debug;
#endif

		explicit frame_allocator_storage(std::size_t capacity)
			: linear(capacity)
#ifndef NDEBUG
			, debug(&linear)
#endif
		{
			egLOG_INFO("memory/frame_allocator", "Frame allocator initialized: {} bytes", capacity);
		}

		std::pmr::memory_resource* resource() noexcept
		{
#ifndef NDEBUG
			return &debug;
#else
			return &linear;
#endif
		}

		void reset() noexcept
		{
#ifndef NDEBUG
			auto leaks = debug.report_leaks();
			if (leaks > 0) {
				egLOG_WARN("memory/frame_allocator", "Frame allocator: {} leaked allocations before reset", leaks);
			}
#endif
			linear.reset();
		}
	};

	frame_allocator_storage& get_storage()
	{
		thread_local frame_allocator_storage storage(default_frame_capacity);
		return storage;
	}

} // anonymous namespace

namespace ds {

	std::pmr::memory_resource* frame_allocator()
	{
		return get_storage().resource();
	}

	void frame_allocator_reset()
	{
		get_storage().reset();
	}

} // namespace ds
