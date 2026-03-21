#pragma once
#include <memory_resource>
#include <cstddef>
#include <cstdint>

namespace ds {

	/// Stack (LIFO) allocator backed by a contiguous buffer.
	/// Each allocation stores a small header with its size, enabling
	/// O(1) deallocation when freed in LIFO order. Out-of-order
	/// deallocations are silently ignored (no-op).
	///
	/// Supports save/restore via markers:
	///   ds::stack_resource stack(4096);
	///   auto m = stack.marker();
	///   void* p = stack.allocate(128, 8);
	///   stack.reset_to_marker(m);  // rewinds past p
	class stack_resource : public std::pmr::memory_resource
	{
	public:
		using marker = std::size_t;

		/// Allocate a backing buffer of \p capacity bytes from \p upstream.
		explicit stack_resource(
			std::size_t capacity,
			std::pmr::memory_resource* upstream = std::pmr::get_default_resource());

		/// Use an externally-owned buffer. No upstream — exhaustion throws.
		stack_resource(void* buffer, std::size_t size);

		~stack_resource() override;

		stack_resource(const stack_resource&) = delete;
		stack_resource& operator=(const stack_resource&) = delete;

		/// Returns current position (offset from buffer start).
		[[nodiscard]] marker get_marker() const noexcept;

		/// Rewinds to a previously saved marker position.
		void reset_to_marker(marker m) noexcept;

		/// Reset bump pointer to the beginning. O(1).
		void reset() noexcept;

		/// Bytes currently in use (including alignment padding and headers).
		[[nodiscard]] std::size_t used() const noexcept;

		/// Total capacity of the backing buffer.
		[[nodiscard]] std::size_t capacity() const noexcept;

	protected:
		void* do_allocate(std::size_t bytes, std::size_t alignment) override;
		void  do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
		bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

	private:
		/// Header stored immediately before each user allocation.
		struct alloc_header
		{
			/// Offset from buffer start before this allocation
			/// (before header and alignment padding).
			std::size_t previous_offset;
			/// Offset from buffer start at the end of this allocation's
			/// user data. Used to verify LIFO order on deallocation.
			std::size_t end_offset;
		};

		std::byte*                  buffer_      = nullptr;
		std::size_t                 capacity_    = 0;
		std::size_t                 offset_      = 0;
		std::pmr::memory_resource*  upstream_    = nullptr;
		bool                        owns_buffer_ = false;
	};

} // namespace ds
