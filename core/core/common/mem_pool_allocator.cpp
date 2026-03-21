#include "mem_pool_allocator.h"
#include <new>
#include <algorithm>
#include "engine_assert.h"

namespace ds {

	pool_resource::pool_resource(
		std::size_t block_size,
		std::size_t block_count,
		std::pmr::memory_resource* upstream)
		: block_size_(std::max(block_size, sizeof(free_node)))
		, initial_block_count_(block_count)
		, upstream_(upstream)
	{
		ASSERT_MSG(block_size > 0, "pool_resource: block_size must be > 0");
		ASSERT_MSG(block_count > 0, "pool_resource: block_count must be > 0");
		ASSERT_MSG(upstream != nullptr, "pool_resource: upstream must not be null");

		grow();
	}

	pool_resource::~pool_resource()
	{
		for (auto& chunk : chunks_) {
			upstream_->deallocate(chunk.data, block_size_ * chunk.block_count, alignof(std::max_align_t));
		}
	}

	void pool_resource::build_free_list(std::byte* chunk_data, std::size_t count) noexcept
	{
		// Build free-list by linking blocks from last to first so that
		// allocation order matches memory order (first block allocated first).
		for (std::size_t i = count; i > 0; --i) {
			auto* node = reinterpret_cast<free_node*>(chunk_data + (i - 1) * block_size_);
			node->next = free_list_;
			free_list_ = node;
		}
	}

	void pool_resource::grow()
	{
		std::size_t count = initial_block_count_;
		auto* data = static_cast<std::byte*>(
			upstream_->allocate(block_size_ * count, alignof(std::max_align_t)));

		chunks_.push_back({data, count});
		total_blocks_ += count;
		free_blocks_ += count;

		build_free_list(data, count);
	}

	void* pool_resource::do_allocate(std::size_t bytes, std::size_t alignment)
	{
		ASSERT_MSG(bytes <= block_size_,
			"pool_resource: requested size exceeds block_size");
		ASSERT_MSG(alignment <= block_size_,
			"pool_resource: requested alignment exceeds block_size");

		if (free_list_ == nullptr) {
			grow();
		}

		free_node* node = free_list_;
		free_list_ = node->next;
		--free_blocks_;
		return static_cast<void*>(node);
	}

	void pool_resource::do_deallocate(void* p, std::size_t /*bytes*/, std::size_t /*alignment*/)
	{
		auto* node = static_cast<free_node*>(p);
		node->next = free_list_;
		free_list_ = node;
		++free_blocks_;
	}

	bool pool_resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
	{
		return this == &other;
	}

	void pool_resource::reset() noexcept
	{
		free_list_ = nullptr;
		free_blocks_ = 0;

		for (auto& chunk : chunks_) {
			free_blocks_ += chunk.block_count;
			build_free_list(chunk.data, chunk.block_count);
		}
	}

	std::size_t pool_resource::block_size() const noexcept
	{
		return block_size_;
	}

	std::size_t pool_resource::total_blocks() const noexcept
	{
		return total_blocks_;
	}

	std::size_t pool_resource::free_blocks() const noexcept
	{
		return free_blocks_;
	}

} // namespace ds
