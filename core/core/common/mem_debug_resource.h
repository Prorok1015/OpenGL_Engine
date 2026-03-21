#pragma once
#include <memory_resource>
#include <cstddef>
#include <cstdint>

#ifndef NDEBUG
#include <unordered_map>
#endif

namespace ds {

	/// Debug wrapper around any `std::pmr::memory_resource`.
	/// In debug builds: adds guard bytes (canary) around each allocation,
	/// tracks active allocations, detects buffer overruns and double-frees.
	/// In release builds: pure pass-through to upstream (zero overhead).
	class debug_resource : public std::pmr::memory_resource
	{
	public:
		explicit debug_resource(std::pmr::memory_resource* upstream);
		~debug_resource() override;

		debug_resource(const debug_resource&) = delete;
		debug_resource& operator=(const debug_resource&) = delete;

		/// Log all still-active allocations. Returns leak count.
		[[nodiscard]] std::size_t report_leaks() const;

		/// Number of currently tracked allocations.
		[[nodiscard]] std::size_t active_allocation_count() const noexcept;

		/// Total user-requested bytes of active allocations (excludes guards).
		[[nodiscard]] std::size_t active_allocation_bytes() const noexcept;

	protected:
		void* do_allocate(std::size_t bytes, std::size_t alignment) override;
		void  do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
		bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

	private:
		static constexpr std::size_t guard_size_ = 16;
		static constexpr std::byte canary_byte_{0xCD};

		std::pmr::memory_resource* upstream_;

#ifndef NDEBUG
		struct alloc_info
		{
			std::size_t user_bytes;
			std::size_t total_bytes;
			void* raw_ptr;
		};

		std::unordered_map<void*, alloc_info> allocations_;

		void fill_guard(std::byte* ptr) const noexcept;
		bool check_guard(const std::byte* ptr) const noexcept;
#endif
	};

} // namespace ds
