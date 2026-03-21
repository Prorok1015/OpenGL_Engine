#pragma once
#include <memory_resource>
#include <cstddef>
#include <cstdint>

namespace ds {

	/// Linear (bump) allocator backed by a contiguous buffer.
	/// Allocations advance a pointer; deallocations are no-ops.
	/// Call reset() to reclaim all memory in O(1).
	///
	/// Intended for per-frame scratch data:
	///   ds::linear_resource frame_mem(1024 * 1024);
	///   std::pmr::vector<int> v(&frame_mem);
	///   // ... use v ...
	///   frame_mem.reset();           // O(1), ready for next frame
	class linear_resource : public std::pmr::memory_resource
	{
	public:
		/// Allocate a backing buffer of \p capacity bytes from \p upstream.
		explicit linear_resource(
			std::size_t capacity,
			std::pmr::memory_resource* upstream = std::pmr::get_default_resource());

		/// Use an externally-owned buffer. No upstream — exhaustion throws.
		linear_resource(void* buffer, std::size_t size);

		~linear_resource() override;

		linear_resource(const linear_resource&) = delete;
		linear_resource& operator=(const linear_resource&) = delete;

		/// Reset bump pointer to the beginning. O(1).
		void reset() noexcept;

		/// Bytes currently in use (including alignment padding).
		[[nodiscard]] std::size_t used() const noexcept;

		/// Total capacity of the backing buffer.
		[[nodiscard]] std::size_t capacity() const noexcept;

	protected:
		void* do_allocate(std::size_t bytes, std::size_t alignment) override;
		void  do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
		bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

	private:
		std::byte*                  buffer_      = nullptr;
		std::size_t                 capacity_    = 0;
		std::size_t                 offset_      = 0;
		std::pmr::memory_resource*  upstream_    = nullptr;
		bool                        owns_buffer_ = false;
	};

} // namespace ds
