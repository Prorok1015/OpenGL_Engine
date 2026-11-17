#include <snk/common/mem_linear_allocator.h>
#include <new>
#include <cstring>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

namespace ds {

	linear_resource::linear_resource(
		std::size_t capacity,
		std::pmr::memory_resource* upstream)
		: capacity_(capacity)
		, upstream_(upstream)
		, owns_buffer_(true)
	{
		ASSERT_MSG(capacity > 0, "linear_resource: capacity must be > 0");
		ASSERT_MSG(upstream != nullptr, "linear_resource: upstream must not be null");
		buffer_ = static_cast<std::byte*>(upstream_->allocate(capacity_, alignof(std::max_align_t)));
	}

	linear_resource::linear_resource(void* buffer, std::size_t size)
		: buffer_(static_cast<std::byte*>(buffer))
		, capacity_(size)
		, upstream_(nullptr)
		, owns_buffer_(false)
	{
		ASSERT_MSG(buffer != nullptr, "linear_resource: buffer must not be null");
		ASSERT_MSG(size > 0, "linear_resource: size must be > 0");
	}

	linear_resource::~linear_resource()
	{
		release_overflow();
		if (owns_buffer_ && buffer_) {
			upstream_->deallocate(buffer_, capacity_, alignof(std::max_align_t));
		}
	}

	void linear_resource::release_overflow() noexcept
	{
		for (const auto& block : overflow_) {
			upstream_->deallocate(block.ptr, block.bytes, block.alignment);
		}
		overflow_.clear();
	}

	void* linear_resource::do_allocate(std::size_t bytes, std::size_t alignment)
	{
		// Compute aligned offset
		std::uintptr_t current = reinterpret_cast<std::uintptr_t>(buffer_ + offset_);
		std::uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
		std::size_t padding = static_cast<std::size_t>(aligned - current);
		std::size_t remaining = capacity_ - offset_;

		if (padding <= remaining && bytes <= remaining - padding) {
			offset_ += padding + bytes;
			return reinterpret_cast<void*>(aligned);
		}

		// Buffer exhausted — fall back to upstream if available. The block is tracked so that
		// reset()/destructor can free it; deallocate() stays a no-op.
		if (upstream_) {
			if (!warned_) {
				warned_ = true;
				egLOG_WARN("memory", "linear_resource overflow to upstream: capacity {} bytes, high-water {} bytes, requested {} bytes",
					capacity_, offset_, bytes);
			}
			if (overflow_.size() == overflow_.capacity()) {
				overflow_.reserve(overflow_.empty() ? 8 : overflow_.size() * 2);
			}
			void* p = upstream_->allocate(bytes, alignment);
			overflow_.push_back({ p, bytes, alignment }); // capacity reserved above, cannot throw
			return p;
		}

		throw std::bad_alloc();
	}

	void linear_resource::do_deallocate(void* /*p*/, std::size_t /*bytes*/, std::size_t /*alignment*/)
	{
		// no-op by design
	}

	bool linear_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
	{
		return this == &other;
	}

	void linear_resource::reset() noexcept
	{
		release_overflow();
		offset_ = 0;
	}

	std::size_t linear_resource::overflow_block_count() const noexcept
	{
		return overflow_.size();
	}

	std::size_t linear_resource::used() const noexcept
	{
		return offset_;
	}

	std::size_t linear_resource::capacity() const noexcept
	{
		return capacity_;
	}

} // namespace ds
