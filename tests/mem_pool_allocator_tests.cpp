#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <vector>
#include "mem_pool_allocator.h"

BOOST_AUTO_TEST_SUITE(MemPoolAllocatorTests)

// --- Construction ---

BOOST_AUTO_TEST_CASE(ConstructWithBlockSizeAndCount) {
	ds::pool_resource pool(64, 10);
	BOOST_TEST(pool.block_size() >= 64u);
	BOOST_TEST(pool.total_blocks() == 10u);
	BOOST_TEST(pool.free_blocks() == 10u);
}

BOOST_AUTO_TEST_CASE(SmallBlockSizePromotedToPointerSize) {
	// block_size < sizeof(void*) must be promoted internally
	ds::pool_resource pool(1, 8);
	BOOST_TEST(pool.block_size() >= sizeof(void*));
	BOOST_TEST(pool.total_blocks() == 8u);
}

// --- Basic allocation ---

BOOST_AUTO_TEST_CASE(SingleAllocation) {
	ds::pool_resource pool(64, 10);
	void* p = pool.allocate(64, 1);
	BOOST_TEST(p != nullptr);
	BOOST_TEST(pool.free_blocks() == 9u);
}

BOOST_AUTO_TEST_CASE(MultipleAllocations) {
	ds::pool_resource pool(32, 4);
	void* a = pool.allocate(32, 1);
	void* b = pool.allocate(32, 1);
	void* c = pool.allocate(32, 1);
	BOOST_TEST(a != nullptr);
	BOOST_TEST(b != nullptr);
	BOOST_TEST(c != nullptr);
	BOOST_TEST(a != b);
	BOOST_TEST(b != c);
	BOOST_TEST(pool.free_blocks() == 1u);
}

BOOST_AUTO_TEST_CASE(AllocateAllBlocks) {
	ds::pool_resource pool(16, 5);
	std::vector<void*> ptrs;
	for (int i = 0; i < 5; ++i) {
		ptrs.push_back(pool.allocate(16, 1));
	}
	BOOST_TEST(pool.free_blocks() == 0u);
	// All pointers must be unique
	for (std::size_t i = 0; i < ptrs.size(); ++i) {
		for (std::size_t j = i + 1; j < ptrs.size(); ++j) {
			BOOST_TEST(ptrs[i] != ptrs[j]);
		}
	}
}

// --- Deallocation ---

BOOST_AUTO_TEST_CASE(DeallocateReturnsFreeBlock) {
	ds::pool_resource pool(64, 4);
	void* p = pool.allocate(64, 1);
	BOOST_TEST(pool.free_blocks() == 3u);
	pool.deallocate(p, 64, 1);
	BOOST_TEST(pool.free_blocks() == 4u);
}

BOOST_AUTO_TEST_CASE(DeallocatedBlockIsReused) {
	ds::pool_resource pool(64, 1);
	void* p1 = pool.allocate(64, 1);
	pool.deallocate(p1, 64, 1);
	void* p2 = pool.allocate(64, 1);
	// After deallocate + allocate, we should get the same block back
	BOOST_TEST(p1 == p2);
}

BOOST_AUTO_TEST_CASE(AllocDeallocCycles) {
	ds::pool_resource pool(32, 2);
	for (int cycle = 0; cycle < 100; ++cycle) {
		void* a = pool.allocate(32, 1);
		void* b = pool.allocate(32, 1);
		pool.deallocate(b, 32, 1);
		pool.deallocate(a, 32, 1);
	}
	BOOST_TEST(pool.free_blocks() == 2u);
	BOOST_TEST(pool.total_blocks() == 2u);
}

// --- Growth (exhaustion → new chunk) ---

BOOST_AUTO_TEST_CASE(GrowsWhenExhausted) {
	ds::pool_resource pool(32, 2);
	pool.allocate(32, 1);
	pool.allocate(32, 1);
	BOOST_TEST(pool.free_blocks() == 0u);
	// Next allocation triggers growth
	void* p = pool.allocate(32, 1);
	BOOST_TEST(p != nullptr);
	BOOST_TEST(pool.total_blocks() == 4u);
	BOOST_TEST(pool.free_blocks() == 1u);
}

BOOST_AUTO_TEST_CASE(MultipleGrowths) {
	ds::pool_resource pool(16, 1);
	for (int i = 0; i < 10; ++i) {
		void* p = pool.allocate(16, 1);
		BOOST_TEST(p != nullptr);
	}
	BOOST_TEST(pool.total_blocks() == 10u);
	BOOST_TEST(pool.free_blocks() == 0u);
}

// --- Reset ---

BOOST_AUTO_TEST_CASE(ResetRestoresAllBlocks) {
	ds::pool_resource pool(64, 8);
	for (int i = 0; i < 8; ++i) {
		pool.allocate(64, 1);
	}
	BOOST_TEST(pool.free_blocks() == 0u);
	pool.reset();
	BOOST_TEST(pool.free_blocks() == 8u);
	BOOST_TEST(pool.total_blocks() == 8u);
}

BOOST_AUTO_TEST_CASE(CanAllocateAfterReset) {
	ds::pool_resource pool(32, 4);
	for (int i = 0; i < 4; ++i) {
		pool.allocate(32, 1);
	}
	pool.reset();
	// Should be able to allocate all blocks again
	for (int i = 0; i < 4; ++i) {
		void* p = pool.allocate(32, 1);
		BOOST_TEST(p != nullptr);
	}
	BOOST_TEST(pool.free_blocks() == 0u);
}

BOOST_AUTO_TEST_CASE(ResetAfterGrowthIncludesAllChunks) {
	ds::pool_resource pool(32, 2);
	// Allocate 4 blocks → triggers 1 growth
	for (int i = 0; i < 4; ++i) {
		pool.allocate(32, 1);
	}
	BOOST_TEST(pool.total_blocks() == 4u);
	pool.reset();
	BOOST_TEST(pool.free_blocks() == 4u);
}

// --- Identity comparison ---

BOOST_AUTO_TEST_CASE(IsEqualSameInstance) {
	ds::pool_resource pool(32, 4);
	BOOST_TEST(pool.is_equal(pool));
}

BOOST_AUTO_TEST_CASE(IsEqualDifferentInstance) {
	ds::pool_resource a(32, 4);
	ds::pool_resource b(32, 4);
	BOOST_TEST(!a.is_equal(b));
}

// --- Accessor consistency ---

BOOST_AUTO_TEST_CASE(BlockSizeAccessor) {
	ds::pool_resource pool(128, 2);
	BOOST_TEST(pool.block_size() >= 128u);
}

BOOST_AUTO_TEST_CASE(FreeBlocksTrackingDuringMixedOps) {
	ds::pool_resource pool(64, 4);
	BOOST_TEST(pool.free_blocks() == 4u);

	void* a = pool.allocate(64, 1);
	BOOST_TEST(pool.free_blocks() == 3u);

	void* b = pool.allocate(64, 1);
	BOOST_TEST(pool.free_blocks() == 2u);

	pool.deallocate(a, 64, 1);
	BOOST_TEST(pool.free_blocks() == 3u);

	void* c = pool.allocate(64, 1);
	BOOST_TEST(pool.free_blocks() == 2u);

	pool.deallocate(b, 64, 1);
	pool.deallocate(c, 64, 1);
	BOOST_TEST(pool.free_blocks() == 4u);
}

// --- Edge cases ---

BOOST_AUTO_TEST_CASE(AllocateSmallerThanBlockSize) {
	ds::pool_resource pool(128, 4);
	// Requesting fewer bytes than block_size is fine
	void* p = pool.allocate(1, 1);
	BOOST_TEST(p != nullptr);
}

BOOST_AUTO_TEST_CASE(AllocateExactBlockSize) {
	ds::pool_resource pool(64, 2);
	void* p = pool.allocate(64, 1);
	BOOST_TEST(p != nullptr);
}

BOOST_AUTO_TEST_CASE(WriteThenReadBlock) {
	// Verify allocated memory is usable for read/write
	ds::pool_resource pool(sizeof(int) * 4, 4);
	auto* arr = static_cast<int*>(pool.allocate(sizeof(int) * 4, alignof(int)));
	arr[0] = 42;
	arr[1] = 99;
	arr[2] = -1;
	arr[3] = 0;
	BOOST_TEST(arr[0] == 42);
	BOOST_TEST(arr[1] == 99);
	BOOST_TEST(arr[2] == -1);
	BOOST_TEST(arr[3] == 0);
}

BOOST_AUTO_TEST_SUITE_END()
