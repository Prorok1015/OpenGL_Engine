#include "mem_debug_resource.h"
#include <cstring>
#include "engine_assert.h"
#include "logger/engine_log.h"

namespace ds {

	debug_resource::debug_resource(std::pmr::memory_resource* upstream)
		: upstream_(upstream)
	{
		ASSERT_MSG(upstream != nullptr, "debug_resource: upstream must not be null");
	}

	debug_resource::~debug_resource()
	{
#ifndef NDEBUG
		auto leaks = report_leaks();
		ASSERT_MSG(leaks == 0, "debug_resource: destroyed with active allocations (memory leak)");
#endif
	}

	void* debug_resource::do_allocate(std::size_t bytes, std::size_t alignment)
	{
#ifndef NDEBUG
		// Layout: [front_guard | padding | user_data | back_guard]
		// Padding ensures user_ptr is aligned to the requested alignment.
		std::size_t effective_align = std::max(alignment, static_cast<std::size_t>(1));
		// After front guard, we may need up to (alignment - 1) extra bytes for padding
		std::size_t max_padding = effective_align - 1;
		std::size_t total = guard_size_ + max_padding + bytes + guard_size_;

		void* raw = upstream_->allocate(total, effective_align);
		std::byte* base = static_cast<std::byte*>(raw);

		// Compute aligned user pointer after front guard
		std::uintptr_t after_guard = reinterpret_cast<std::uintptr_t>(base + guard_size_);
		std::uintptr_t aligned = (after_guard + effective_align - 1) & ~(effective_align - 1);
		std::byte* user_ptr = reinterpret_cast<std::byte*>(aligned);

		// Place front guard immediately before user_ptr
		fill_guard(user_ptr - guard_size_);
		// Place back guard immediately after user data
		fill_guard(user_ptr + bytes);

		// Track: store raw pointer and sizes for deallocation
		allocations_[user_ptr] = alloc_info{bytes, total, raw};

		return user_ptr;
#else
		return upstream_->allocate(bytes, alignment);
#endif
	}

	void debug_resource::do_deallocate(void* p, std::size_t bytes, std::size_t alignment)
	{
#ifndef NDEBUG
		auto it = allocations_.find(p);
		ASSERT_MSG(it != allocations_.end(),
			"debug_resource: deallocating unknown pointer (double-free or invalid free)");

		const auto& info = it->second;
		std::byte* user_ptr = static_cast<std::byte*>(p);

		// Verify front guard (immediately before user_ptr)
		ASSERT_MSG(check_guard(user_ptr - guard_size_),
			"debug_resource: front guard corrupted (buffer underrun detected)");

		// Verify back guard (immediately after user data)
		ASSERT_MSG(check_guard(user_ptr + info.user_bytes),
			"debug_resource: back guard corrupted (buffer overrun detected)");

		std::size_t total = info.total_bytes;
		void* raw = info.raw_ptr;
		std::size_t stored_alignment = std::max(alignment, static_cast<std::size_t>(1));

		allocations_.erase(it);

		upstream_->deallocate(raw, total, stored_alignment);
#else
		upstream_->deallocate(p, bytes, alignment);
#endif
	}

	bool debug_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
	{
		return this == &other;
	}

	std::size_t debug_resource::report_leaks() const
	{
#ifndef NDEBUG
		if (allocations_.empty()) {
			return 0;
		}

		egLOG_WARN("memory", "debug_resource: {} active allocation(s) leaked:", allocations_.size());
		for (const auto& [ptr, info] : allocations_) {
			egLOG_WARN("memory", "  leaked {} bytes at {}", info.user_bytes, ptr);
		}

		return allocations_.size();
#else
		return 0;
#endif
	}

	std::size_t debug_resource::active_allocation_count() const noexcept
	{
#ifndef NDEBUG
		return allocations_.size();
#else
		return 0;
#endif
	}

	std::size_t debug_resource::active_allocation_bytes() const noexcept
	{
#ifndef NDEBUG
		std::size_t total = 0;
		for (const auto& [ptr, info] : allocations_) {
			total += info.user_bytes;
		}
		return total;
#else
		return 0;
#endif
	}

#ifndef NDEBUG
	void debug_resource::fill_guard(std::byte* ptr) const noexcept
	{
		std::memset(ptr, static_cast<int>(canary_byte_), guard_size_);
	}

	bool debug_resource::check_guard(const std::byte* ptr) const noexcept
	{
		for (std::size_t i = 0; i < guard_size_; ++i) {
			if (ptr[i] != canary_byte_) {
				return false;
			}
		}
		return true;
	}
#endif

} // namespace ds
