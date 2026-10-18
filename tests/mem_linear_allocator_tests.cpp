#include <boost/test/unit_test.hpp>
#include <vector>
#include <memory_resource>
#include <snk/common/mem_linear_allocator.h>

BOOST_AUTO_TEST_SUITE(MemLinearAllocatorTests)

// --- Construction ---

BOOST_AUTO_TEST_CASE(ConstructWithCapacity) {
	ds::linear_resource alloc(1024);
	BOOST_TEST(alloc.capacity() == 1024u);
	BOOST_TEST(alloc.used() == 0u);
}

BOOST_AUTO_TEST_CASE(ConstructWithExternalBuffer) {
	alignas(std::max_align_t) std::byte buf[512];
	ds::linear_resource alloc(buf, sizeof(buf));
	BOOST_TEST(alloc.capacity() == 512u);
	BOOST_TEST(alloc.used() == 0u);
}

// --- Basic allocation ---

BOOST_AUTO_TEST_CASE(SingleAllocation) {
	ds::linear_resource alloc(1024);
	void* p = alloc.allocate(64, alignof(int));
	BOOST_TEST(p != nullptr);
	BOOST_TEST(alloc.used() >= 64u);
}

BOOST_AUTO_TEST_CASE(MultipleAllocations) {
	ds::linear_resource alloc(1024);
	void* a = alloc.allocate(100, 1);
	void* b = alloc.allocate(200, 1);
	BOOST_TEST(a != nullptr);
	BOOST_TEST(b != nullptr);
	BOOST_TEST(a != b);
	BOOST_TEST(alloc.used() >= 300u);
}

// --- Alignment ---

BOOST_AUTO_TEST_CASE(AllocationRespects16ByteAlignment) {
	ds::linear_resource alloc(4096);
	// Burn 1 byte to shift the internal pointer off-alignment
	(void)alloc.allocate(1, 1);
	void* p = alloc.allocate(32, 16);
	auto addr = reinterpret_cast<std::uintptr_t>(p);
	BOOST_TEST((addr % 16) == 0u);
}

BOOST_AUTO_TEST_CASE(AllocationRespects64ByteAlignment) {
	ds::linear_resource alloc(4096);
	(void)alloc.allocate(3, 1);
	void* p = alloc.allocate(64, 64);
	auto addr = reinterpret_cast<std::uintptr_t>(p);
	BOOST_TEST((addr % 64) == 0u);
}

// --- Reset ---

BOOST_AUTO_TEST_CASE(ResetRestoresUsedToZero) {
	ds::linear_resource alloc(1024);
	(void)alloc.allocate(512, 1);
	BOOST_TEST(alloc.used() > 0u);
	alloc.reset();
	BOOST_TEST(alloc.used() == 0u);
}

BOOST_AUTO_TEST_CASE(CanAllocateAfterReset) {
	ds::linear_resource alloc(256);
	(void)alloc.allocate(200, 1);
	alloc.reset();
	void* p = alloc.allocate(200, 1);
	BOOST_TEST(p != nullptr);
	BOOST_TEST(alloc.used() >= 200u);
}

// --- Overflow with external buffer (no upstream) ---

BOOST_AUTO_TEST_CASE(ExternalBufferThrowsOnOverflow) {
	alignas(std::max_align_t) std::byte buf[64];
	ds::linear_resource alloc(buf, sizeof(buf));
	// Fill the buffer
	(void)alloc.allocate(64, 1);
	// Next allocation must throw — no upstream
	BOOST_CHECK_THROW((void)alloc.allocate(1, 1), std::bad_alloc);
}

// --- Overflow with upstream fallback ---

BOOST_AUTO_TEST_CASE(UpstreamFallbackOnOverflow) {
	ds::linear_resource alloc(64);
	(void)alloc.allocate(64, 1);
	// This should NOT throw — falls back to upstream (default resource)
	void* p = nullptr;
	BOOST_CHECK_NO_THROW(p = alloc.allocate(32, 1));
	BOOST_TEST(p != nullptr);
	// The overflow block is owned by the resource and freed on reset()/destruction.
	BOOST_TEST(alloc.overflow_block_count() == 1u);
}

namespace {

	// Counts live upstream allocations/bytes so leaks and double frees are observable.
	class counting_resource : public std::pmr::memory_resource
	{
	public:
		std::size_t live_blocks = 0;
		std::size_t live_bytes = 0;
		std::size_t allocs = 0;
		std::size_t deallocs = 0;
		std::size_t last_alignment = 0;

	protected:
		void* do_allocate(std::size_t bytes, std::size_t alignment) override
		{
			++allocs;
			++live_blocks;
			live_bytes += bytes;
			last_alignment = alignment;
			return std::pmr::new_delete_resource()->allocate(bytes, alignment);
		}
		void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override
		{
			++deallocs;
			BOOST_REQUIRE(live_blocks > 0);
			--live_blocks;
			live_bytes -= bytes;
			std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
		}
		bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override
		{
			return this == &o;
		}
	};

}

BOOST_AUTO_TEST_CASE(ResetReleasesOverflowToUpstream) {
	counting_resource up;
	{
		ds::linear_resource alloc(64, &up);
		BOOST_TEST(up.live_blocks == 1u); // backing buffer only
		(void)alloc.allocate(64, 1);
		(void)alloc.allocate(100, 8);
		(void)alloc.allocate(200, 16);
		BOOST_TEST(up.live_blocks == 3u);
		BOOST_TEST(alloc.overflow_block_count() == 2u);

		alloc.reset();
		BOOST_TEST(up.live_blocks == 1u);
		BOOST_TEST(alloc.overflow_block_count() == 0u);
		BOOST_TEST(alloc.used() == 0u);

		// Second reset must not double free.
		alloc.reset();
		BOOST_TEST(up.deallocs == 2u);
	}
	BOOST_TEST(up.live_blocks == 0u);
	BOOST_TEST(up.allocs == up.deallocs);
}

BOOST_AUTO_TEST_CASE(RepeatedFillResetDoesNotGrowUpstream) {
	counting_resource up;
	ds::linear_resource alloc(256, &up);
	for (int cycle = 0; cycle < 100; ++cycle) {
		for (int i = 0; i < 20; ++i) {
			(void)alloc.allocate(64, 8);
		}
		BOOST_TEST(up.live_blocks <= 1u + 20u);
		alloc.reset();
		BOOST_REQUIRE(up.live_blocks == 1u);
		BOOST_REQUIRE(up.live_bytes == 256u);
	}
}

BOOST_AUTO_TEST_CASE(OverflowBlockHonoursAlignmentAndDeallocateIsSafe) {
	counting_resource up;
	ds::linear_resource alloc(32, &up);
	(void)alloc.allocate(32, 1);
	void* p = alloc.allocate(64, 64);
	BOOST_TEST(reinterpret_cast<std::uintptr_t>(p) % 64 == 0u);
	BOOST_TEST(up.last_alignment == 64u);
	// Caller-side deallocate stays a no-op: no double free when reset() releases the block.
	alloc.deallocate(p, 64, 64);
	BOOST_TEST(up.live_blocks == 2u);
	alloc.reset();
	BOOST_TEST(up.live_blocks == 1u);
}

BOOST_AUTO_TEST_CASE(PmrContainerOverflowIsReclaimedOnReset) {
	counting_resource up;
	ds::linear_resource alloc(128, &up);
	{
		std::pmr::vector<int> v(&alloc);
		for (int i = 0; i < 1000; ++i) {
			v.push_back(i);
		}
	}
	BOOST_TEST(up.live_blocks > 1u);
	alloc.reset();
	BOOST_TEST(up.live_blocks == 1u);
}

// --- Deallocate is a no-op ---

BOOST_AUTO_TEST_CASE(DeallocateDoesNotFreeMemory) {
	ds::linear_resource alloc(1024);
	void* p = alloc.allocate(128, 1);
	auto used_before = alloc.used();
	alloc.deallocate(p, 128, 1);
	// used() should not change — deallocate is no-op
	BOOST_TEST(alloc.used() == used_before);
}

// --- Identity comparison ---

BOOST_AUTO_TEST_CASE(IsEqualSameInstance) {
	ds::linear_resource alloc(256);
	BOOST_TEST(alloc.is_equal(alloc));
}

BOOST_AUTO_TEST_CASE(IsEqualDifferentInstance) {
	ds::linear_resource a(256);
	ds::linear_resource b(256);
	BOOST_TEST(!a.is_equal(b));
}

// --- Integration with std::pmr::vector ---

BOOST_AUTO_TEST_CASE(PmrVectorWorks) {
	ds::linear_resource alloc(4096);
	std::pmr::vector<int> v(&alloc);
	v.reserve(100);
	for (int i = 0; i < 100; ++i) {
		v.push_back(i);
	}
	BOOST_TEST(v.size() == 100u);
	for (int i = 0; i < 100; ++i) {
		BOOST_TEST(v[i] == i);
	}
	BOOST_TEST(alloc.used() > 0u);
}

BOOST_AUTO_TEST_CASE(PmrVectorWorksAfterReset) {
	ds::linear_resource alloc(4096);
	{
		std::pmr::vector<int> v(&alloc);
		v.assign({1, 2, 3, 4, 5});
		BOOST_TEST(v.size() == 5u);
	}
	alloc.reset();
	// Reuse memory for a new vector
	{
		std::pmr::vector<double> v(&alloc);
		v.assign({1.0, 2.0, 3.0});
		BOOST_TEST(v.size() == 3u);
	}
}

// --- Edge cases ---

BOOST_AUTO_TEST_CASE(ZeroBytesAllocation) {
	ds::linear_resource alloc(256);
	// Zero-size allocation should succeed (std::pmr allows it)
	void* p = alloc.allocate(0, 1);
	BOOST_TEST(p != nullptr);
}

BOOST_AUTO_TEST_CASE(ExactFitAllocation) {
	alignas(std::max_align_t) std::byte buf[128];
	ds::linear_resource alloc(buf, sizeof(buf));
	// Allocate exactly the full capacity
	void* p = alloc.allocate(128, 1);
	BOOST_TEST(p != nullptr);
	BOOST_TEST(alloc.used() == 128u);
}

BOOST_AUTO_TEST_SUITE_END()
