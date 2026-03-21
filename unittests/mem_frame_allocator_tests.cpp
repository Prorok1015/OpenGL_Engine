#include <boost/test/unit_test.hpp>
#include <vector>
#include <memory_resource>
#include "mem_allocator.h"

BOOST_AUTO_TEST_SUITE(MemFrameAllocatorTests)

// --- Basic API ---

BOOST_AUTO_TEST_CASE(FrameAllocatorReturnsNonNull) {
	auto* res = ds::frame_allocator();
	BOOST_TEST(res != nullptr);
}

BOOST_AUTO_TEST_CASE(FrameAllocatorReturnsSamePointer) {
	auto* a = ds::frame_allocator();
	auto* b = ds::frame_allocator();
	BOOST_TEST(a == b);
}

// --- Allocation through resource ---

BOOST_AUTO_TEST_CASE(CanAllocateFromFrameAllocator) {
	ds::frame_allocator_reset();
	auto* res = ds::frame_allocator();
	void* p = res->allocate(128, alignof(int));
	BOOST_TEST(p != nullptr);
	res->deallocate(p, 128, alignof(int));
}

BOOST_AUTO_TEST_CASE(MultipleAllocationsSucceed) {
	ds::frame_allocator_reset();
	auto* res = ds::frame_allocator();
	void* a = res->allocate(64, 1);
	void* b = res->allocate(64, 1);
	void* c = res->allocate(64, 1);
	BOOST_TEST(a != nullptr);
	BOOST_TEST(b != nullptr);
	BOOST_TEST(c != nullptr);
	BOOST_TEST(a != b);
	BOOST_TEST(b != c);
	// Deallocate to keep debug_resource happy
	res->deallocate(a, 64, 1);
	res->deallocate(b, 64, 1);
	res->deallocate(c, 64, 1);
}

// --- Reset between frames ---

BOOST_AUTO_TEST_CASE(ResetAllowsReuse) {
	ds::frame_allocator_reset();
	auto* res = ds::frame_allocator();

	// "Frame 1" — allocate some memory
	void* p1 = res->allocate(256, 1);
	BOOST_TEST(p1 != nullptr);
	res->deallocate(p1, 256, 1);

	// Simulate frame boundary
	ds::frame_allocator_reset();

	// "Frame 2" — should be able to allocate again from the same arena
	void* p2 = res->allocate(256, 1);
	BOOST_TEST(p2 != nullptr);
	res->deallocate(p2, 256, 1);

	ds::frame_allocator_reset();
}

// --- pmr::vector integration ---

BOOST_AUTO_TEST_CASE(PmrVectorWithFrameAllocator) {
	ds::frame_allocator_reset();
	{
		std::pmr::vector<int> v(ds::frame_allocator());
		v.reserve(100);
		for (int i = 0; i < 100; ++i) {
			v.push_back(i * 3);
		}
		BOOST_TEST(v.size() == 100u);
		BOOST_TEST(v[0] == 0);
		BOOST_TEST(v[99] == 297);
	}
	// After vector destruction, reset is safe
	ds::frame_allocator_reset();
}

BOOST_AUTO_TEST_CASE(PmrVectorAcrossResets) {
	ds::frame_allocator_reset();

	// Frame 1
	{
		std::pmr::vector<double> v(ds::frame_allocator());
		v.assign({1.0, 2.0, 3.0, 4.0, 5.0});
		BOOST_TEST(v.size() == 5u);
	}
	ds::frame_allocator_reset();

	// Frame 2 — memory reused
	{
		std::pmr::vector<float> v(ds::frame_allocator());
		v.assign({10.0f, 20.0f, 30.0f});
		BOOST_TEST(v.size() == 3u);
		BOOST_TEST(v[2] == 30.0f);
	}
	ds::frame_allocator_reset();
}

// --- Alignment ---

BOOST_AUTO_TEST_CASE(AlignedAllocationFromFrameAllocator) {
	ds::frame_allocator_reset();
	auto* res = ds::frame_allocator();

	// Burn a byte to shift the internal pointer
	void* burn = res->allocate(1, 1);

	void* p = res->allocate(64, 64);
	auto addr = reinterpret_cast<std::uintptr_t>(p);
	BOOST_TEST((addr % 64) == 0u);

	res->deallocate(burn, 1, 1);
	res->deallocate(p, 64, 64);
	ds::frame_allocator_reset();
}

// --- Stress: many small allocations within a frame ---

BOOST_AUTO_TEST_CASE(ManySmallAllocations) {
	ds::frame_allocator_reset();
	auto* res = ds::frame_allocator();

	constexpr int count = 1000;
	std::vector<void*> ptrs;
	ptrs.reserve(count);

	for (int i = 0; i < count; ++i) {
		void* p = res->allocate(16, alignof(int));
		BOOST_TEST(p != nullptr);
		ptrs.push_back(p);
	}

	// Deallocate all (no-op for linear, but needed for debug_resource tracking)
	for (auto* p : ptrs) {
		res->deallocate(p, 16, alignof(int));
	}

	ds::frame_allocator_reset();
}

BOOST_AUTO_TEST_SUITE_END()
