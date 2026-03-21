#pragma once
#include <memory_resource>

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
	std::pmr::memory_resource* frame_allocator();

	/// Resets the per-frame linear allocator. Must be called once
	/// at the beginning of each frame (before any frame_allocator() usage).
	/// All memory handed out since the previous reset becomes invalid.
	void frame_allocator_reset();

} // namespace ds
