#pragma once
#include <cstddef>
#include <memory_resource>
#include <snk/common/snk_api.h>

namespace ds {

	/// Returns the thread-local per-frame memory resource.
	/// Lazily initialized on first call. The returned pointer is stable
	/// for the lifetime of the calling thread.
	///
	/// Usage:
	///   std::pmr::vector<int> tmp(ds::frame_allocator());
	///
	/// In debug builds the resource is wrapped with debug_resource
	/// for leak/overrun detection.
	SNK_API std::pmr::memory_resource* frame_allocator();

	/// Resets the per-frame linear allocator. Must be called once
	/// at the beginning of each frame (before any frame_allocator() usage).
	/// All memory handed out since the previous reset becomes invalid.
	SNK_API void frame_allocator_reset();

	/// How much of the calling thread's frame allocator is in use.
	struct frame_allocator_stats
	{
		/// Bytes handed out since the last reset (alignment padding included).
		std::size_t used = 0;
		/// The most `used` ever reached on this thread (high-water mark).
		std::size_t peak = 0;
		std::size_t capacity = 0;
		/// Blocks taken from upstream because the buffer ran out; 0 in a healthy frame.
		std::size_t overflow_blocks = 0;
	};
	SNK_API frame_allocator_stats frame_allocator_usage();

} // namespace ds
