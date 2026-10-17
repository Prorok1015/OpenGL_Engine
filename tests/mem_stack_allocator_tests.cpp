#include <boost/test/unit_test.hpp>
#include <vector>
#include <memory_resource>
#include "mem_stack_allocator.h"

BOOST_AUTO_TEST_SUITE(MemStackAllocatorTests)

// --- Construction ---

BOOST_AUTO_TEST_CASE(ConstructWithCapacity) {
	ds::stack_resource alloc(1024);
	BOOST_TEST(alloc.capacity() == 1024u);
	BOOST_TEST(alloc.used() == 0u);
}

BOOST_AUTO_TEST_CASE(ConstructWithExternalBuffer) {
	alignas(std::max_align_t) std::byte buf[512];
	ds::stack_resource alloc(buf, sizeof(buf));
	BOOST_TEST(alloc.capacity() == 512u);
	BOOST_TEST(alloc.used() == 0u);
}

// --- Basic allocation ---

BOOST_AUTO_TEST_CASE(SingleAllocation) {
	ds::stack_resource alloc(1024);
	void* p = alloc.allocate(64, alignof(int));
	BOOST_TEST(p != nullptr);
	BOOST_TEST(alloc.used() >= 64u);
}

BOOST_AUTO_TEST_CASE(MultipleAllocations) {
	ds::stack_resource alloc(4096);
	void* a = alloc.allocate(100, 1);
	void* b = alloc.allocate(200, 1);
	BOOST_TEST(a != nullptr);
	BOOST_TEST(b != nullptr);
	BOOST_TEST(a != b);
	BOOST_TEST(alloc.used() >= 300u);
}

// --- Alignment ---

BOOST_AUTO_TEST_CASE(AllocationRespects16ByteAlignment) {
	ds::stack_resource alloc(4096);
	alloc.allocate(1, 1);
	void* p = alloc.allocate(32, 16);
	auto addr = reinterpret_cast<std::uintptr_t>(p);
	BOOST_TEST((addr % 16) == 0u);
}

BOOST_AUTO_TEST_CASE(AllocationRespects64ByteAlignment) {
	ds::stack_resource alloc(4096);
	alloc.allocate(3, 1);
	void* p = alloc.allocate(64, 64);
	auto addr = reinterpret_cast<std::uintptr_t>(p);
	BOOST_TEST((addr % 64) == 0u);
}

// --- LIFO deallocation ---

BOOST_AUTO_TEST_CASE(DeallocateMostRecentRewinds) {
	ds::stack_resource alloc(1024);
	alloc.allocate(64, 1);
	auto used_after_first = alloc.used();

	void* p = alloc.allocate(128, 1);
	BOOST_TEST(alloc.used() > used_after_first);

	alloc.deallocate(p, 128, 1);
	BOOST_TEST(alloc.used() == used_after_first);
}

BOOST_AUTO_TEST_CASE(DeallocateNonLifoIsNoop) {
	ds::stack_resource alloc(1024);
	void* a = alloc.allocate(64, 1);
	alloc.allocate(128, 1);
	auto used_before = alloc.used();

	// Deallocate first allocation (not LIFO order) — should be no-op
	alloc.deallocate(a, 64, 1);
	BOOST_TEST(alloc.used() == used_before);
}

BOOST_AUTO_TEST_CASE(SequentialLifoDeallocation) {
	ds::stack_resource alloc(4096);
	auto m0 = alloc.used();

	void* a = alloc.allocate(100, 8);
	auto m1 = alloc.used();

	void* b = alloc.allocate(200, 8);
	auto m2 = alloc.used();

	void* c = alloc.allocate(300, 8);

	// Deallocate in reverse order
	alloc.deallocate(c, 300, 8);
	BOOST_TEST(alloc.used() == m2);

	alloc.deallocate(b, 200, 8);
	BOOST_TEST(alloc.used() == m1);

	alloc.deallocate(a, 100, 8);
	BOOST_TEST(alloc.used() == m0);
}

// --- Markers ---

BOOST_AUTO_TEST_CASE(MarkerReturnsCurrentPosition) {
	ds::stack_resource alloc(1024);
	auto m0 = alloc.get_marker();
	BOOST_TEST(m0 == 0u);

	alloc.allocate(64, 1);
	auto m1 = alloc.get_marker();
	BOOST_TEST(m1 > m0);
}

BOOST_AUTO_TEST_CASE(ResetToMarkerRewinds) {
	ds::stack_resource alloc(4096);
	alloc.allocate(100, 1);
	auto m = alloc.get_marker();
	auto used_at_marker = alloc.used();

	alloc.allocate(200, 1);
	alloc.allocate(300, 1);
	BOOST_TEST(alloc.used() > used_at_marker);

	alloc.reset_to_marker(m);
	BOOST_TEST(alloc.used() == used_at_marker);
}

BOOST_AUTO_TEST_CASE(NestedScopeWithMarkers) {
	ds::stack_resource alloc(4096);

	// Outer scope
	alloc.allocate(100, 8);
	auto outer_marker = alloc.get_marker();

	{
		// Inner scope
		alloc.allocate(200, 8);
		auto inner_marker = alloc.get_marker();

		{
			// Innermost scope
			alloc.allocate(300, 8);
			BOOST_TEST(alloc.used() > inner_marker);
		}
		// Rewind innermost
		alloc.reset_to_marker(inner_marker);
		BOOST_TEST(alloc.used() == inner_marker);
	}
	// Rewind inner
	alloc.reset_to_marker(outer_marker);
	BOOST_TEST(alloc.used() == outer_marker);
}

// --- Reset ---

BOOST_AUTO_TEST_CASE(ResetRestoresUsedToZero) {
	ds::stack_resource alloc(1024);
	alloc.allocate(512, 1);
	BOOST_TEST(alloc.used() > 0u);
	alloc.reset();
	BOOST_TEST(alloc.used() == 0u);
}

BOOST_AUTO_TEST_CASE(CanAllocateAfterReset) {
	ds::stack_resource alloc(256);
	alloc.allocate(100, 1);
	alloc.reset();
	void* p = alloc.allocate(100, 1);
	BOOST_TEST(p != nullptr);
	BOOST_TEST(alloc.used() >= 100u);
}

// --- Overflow ---

BOOST_AUTO_TEST_CASE(ExternalBufferThrowsOnOverflow) {
	alignas(std::max_align_t) std::byte buf[64];
	ds::stack_resource alloc(buf, sizeof(buf));
	// Headers take space, so even a small alloc may fill up quickly
	alloc.allocate(32, 1);
	// This should eventually throw
	BOOST_CHECK_THROW(alloc.allocate(64, 1), std::bad_alloc);
}

BOOST_AUTO_TEST_CASE(UpstreamFallbackOnOverflow) {
	ds::stack_resource alloc(64);
	alloc.allocate(32, 1);
	// Falls back to upstream (default resource)
	void* p = nullptr;
	BOOST_CHECK_NO_THROW(p = alloc.allocate(256, 1));
	BOOST_TEST(p != nullptr);
	// Clean up the upstream allocation
	std::pmr::get_default_resource()->deallocate(p, 256, 1);
}

// --- Identity comparison ---

BOOST_AUTO_TEST_CASE(IsEqualSameInstance) {
	ds::stack_resource alloc(256);
	BOOST_TEST(alloc.is_equal(alloc));
}

BOOST_AUTO_TEST_CASE(IsEqualDifferentInstance) {
	ds::stack_resource a(256);
	ds::stack_resource b(256);
	BOOST_TEST(!a.is_equal(b));
}

// --- Integration with std::pmr::vector ---

BOOST_AUTO_TEST_CASE(PmrVectorWorks) {
	ds::stack_resource alloc(8192);
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

// --- Allocate after LIFO dealloc reuses space ---

BOOST_AUTO_TEST_CASE(ReallocateAfterLifoDealloc) {
	ds::stack_resource alloc(4096);
	alloc.allocate(64, 1);
	auto used_after_first = alloc.used();

	void* p = alloc.allocate(128, 1);
	alloc.deallocate(p, 128, 1);
	BOOST_TEST(alloc.used() == used_after_first);

	// Allocate again — should reuse the freed space
	void* q = alloc.allocate(128, 1);
	BOOST_TEST(q != nullptr);
}

// --- Deallocate external pointer is no-op ---

BOOST_AUTO_TEST_CASE(DeallocateExternalPointerIsNoop) {
	ds::stack_resource alloc(1024);
	alloc.allocate(64, 1);
	auto used_before = alloc.used();

	// Some random pointer not from our buffer
	int dummy = 42;
	alloc.deallocate(&dummy, sizeof(int), alignof(int));
	BOOST_TEST(alloc.used() == used_before);
}

// --- Edge: zero-byte allocation ---

BOOST_AUTO_TEST_CASE(ZeroBytesAllocation) {
	ds::stack_resource alloc(256);
	void* p = alloc.allocate(0, 1);
	BOOST_TEST(p != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
