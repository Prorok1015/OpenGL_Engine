#pragma once
#include <memory_resource>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ds {

	/// Fixed-size block pool allocator backed by an intrusive free-list.
	/// Allocations and deallocations are O(1). All blocks have the same size.
	///
	/// Intended for objects of uniform size (ECS components, scene nodes, handles):
	///   ds::pool_resource pool(sizeof(my_component), 1024);
	///   void* p = pool.allocate(sizeof(my_component), alignof(my_component));
	///   pool.deallocate(p, sizeof(my_component), alignof(my_component));
	///   pool.reset();  // all blocks returned to free-list
	class pool_resource : public std::pmr::memory_resource
	{
	public:
		/// Allocate an initial chunk of \p block_count blocks, each \p block_size bytes,
		/// from \p upstream. block_size must be >= sizeof(void*) (enforced internally).
		pool_resource(
			std::size_t block_size,
			std::size_t block_count,
			std::pmr::memory_resource* upstream = std::pmr::get_default_resource());

		~pool_resource() override;

		pool_resource(const pool_resource&) = delete;
		pool_resource& operator=(const pool_resource&) = delete;

		/// Rebuild free-list from all chunks — all blocks become available. O(N).
		void reset() noexcept;

		/// Size of each block in bytes.
		[[nodiscard]] std::size_t block_size() const noexcept;

		/// Total number of blocks across all chunks.
		[[nodiscard]] std::size_t total_blocks() const noexcept;

		/// Number of currently free blocks.
		[[nodiscard]] std::size_t free_blocks() const noexcept;

	protected:
		void* do_allocate(std::size_t bytes, std::size_t alignment) override;
		void  do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
		bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

	private:
		struct free_node
		{
			free_node* next;
		};

		struct chunk_info
		{
			std::byte*  data;
			std::size_t block_count;
		};

		void build_free_list(std::byte* chunk_data, std::size_t count) noexcept;
		void grow();

		std::size_t                 block_size_;
		std::size_t                 initial_block_count_;
		std::size_t                 total_blocks_    = 0;
		std::size_t                 free_blocks_     = 0;
		free_node*                  free_list_       = nullptr;
		std::vector<chunk_info>     chunks_;
		std::pmr::memory_resource*  upstream_        = nullptr;
	};

} // namespace ds
