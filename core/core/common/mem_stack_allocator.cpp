#include "mem_stack_allocator.h"
#include <new>
#include <algorithm>
#include "engine_assert.h"

namespace ds {

	stack_resource::stack_resource(
		std::size_t capacity,
		std::pmr::memory_resource* upstream)
		: capacity_(capacity)
		, upstream_(upstream)
		, owns_buffer_(true)
	{
		ASSERT_MSG(capacity > 0, "stack_resource: capacity must be > 0");
		ASSERT_MSG(upstream != nullptr, "stack_resource: upstream must not be null");
		buffer_ = static_cast<std::byte*>(upstream_->allocate(capacity_, alignof(std::max_align_t)));
	}

	stack_resource::stack_resource(void* buffer, std::size_t size)
		: buffer_(static_cast<std::byte*>(buffer))
		, capacity_(size)
		, upstream_(nullptr)
		, owns_buffer_(false)
	{
		ASSERT_MSG(buffer != nullptr, "stack_resource: buffer must not be null");
		ASSERT_MSG(size > 0, "stack_resource: size must be > 0");
	}

	stack_resource::~stack_resource()
	{
		if (owns_buffer_ && buffer_) {
			upstream_->deallocate(buffer_, capacity_, alignof(std::max_align_t));
		}
	}

	void* stack_resource::do_allocate(std::size_t bytes, std::size_t alignment)
	{
		// Save offset before this allocation for LIFO rewind.
		std::size_t saved_offset = offset_;

		// Layout: [padding] [header] [user_padding] [user_data]
		// The header is placed immediately before the user pointer.
		// We ensure the user pointer is aligned to max(alignment, alignof(alloc_header))
		// so the header (which is smaller or equal) is also naturally aligned.
		std::size_t effective_align = std::max(alignment, alignof(alloc_header));
		std::size_t header_size = sizeof(alloc_header);

		// Minimum position for user pointer: offset_ + header_size, then aligned up.
		std::uintptr_t base = reinterpret_cast<std::uintptr_t>(buffer_ + offset_ + header_size);
		std::uintptr_t user_aligned = (base + effective_align - 1) & ~(effective_align - 1);
		std::size_t total_end = static_cast<std::size_t>(user_aligned - reinterpret_cast<std::uintptr_t>(buffer_)) + bytes;

		if (total_end <= capacity_) {
			// Place header immediately before user pointer.
			alloc_header* header = reinterpret_cast<alloc_header*>(user_aligned - header_size);
			header->previous_offset = saved_offset;
			header->end_offset = total_end;

			offset_ = total_end;
			return reinterpret_cast<void*>(user_aligned);
		}

		// Buffer exhausted — fall back to upstream if available.
		if (upstream_) {
			return upstream_->allocate(bytes, alignment);
		}

		throw std::bad_alloc();
	}

	void stack_resource::do_deallocate(void* p, std::size_t /*bytes*/, std::size_t /*alignment*/)
	{
		auto addr = reinterpret_cast<std::uintptr_t>(p);
		auto buf_start = reinterpret_cast<std::uintptr_t>(buffer_);
		auto buf_end = buf_start + capacity_;

		// Not our allocation (possibly from upstream).
		if (addr < buf_start || addr >= buf_end) {
			return;
		}

		// The header is always stored immediately before the user pointer.
		auto header_addr = addr - sizeof(alloc_header);
		if (header_addr < buf_start) {
			return;
		}

		const alloc_header* header = reinterpret_cast<const alloc_header*>(header_addr);

		// LIFO check: this is the most recent allocation if and only if
		// its end_offset equals the current offset_.
		if (header->end_offset == offset_ && header->previous_offset <= offset_) {
			offset_ = header->previous_offset;
		}
		// Otherwise — not LIFO order, no-op.
	}

	bool stack_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
	{
		return this == &other;
	}

	stack_resource::marker stack_resource::get_marker() const noexcept
	{
		return offset_;
	}

	void stack_resource::reset_to_marker(marker m) noexcept
	{
		ASSERT_MSG(m <= offset_, "stack_resource: marker must be <= current offset");
		offset_ = m;
	}

	void stack_resource::reset() noexcept
	{
		offset_ = 0;
	}

	std::size_t stack_resource::used() const noexcept
	{
		return offset_;
	}

	std::size_t stack_resource::capacity() const noexcept
	{
		return capacity_;
	}

} // namespace ds
