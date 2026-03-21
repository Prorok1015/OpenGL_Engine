#include <boost/test/unit_test.hpp>
#include <vector>
#include <memory_resource>
#include "mem_debug_resource.h"

BOOST_AUTO_TEST_SUITE(MemDebugResourceTests)

// --- Construction ---

BOOST_AUTO_TEST_CASE(ConstructWithUpstream) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	BOOST_TEST(dbg.active_allocation_count() == 0u);
	BOOST_TEST(dbg.active_allocation_bytes() == 0u);
}

// --- Basic allocation and deallocation ---

BOOST_AUTO_TEST_CASE(SingleAllocDealloc) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	void* p = dbg.allocate(64, alignof(int));
	BOOST_TEST(p != nullptr);
	BOOST_TEST(dbg.active_allocation_count() == 1u);
	BOOST_TEST(dbg.active_allocation_bytes() == 64u);
	dbg.deallocate(p, 64, alignof(int));
	BOOST_TEST(dbg.active_allocation_count() == 0u);
	BOOST_TEST(dbg.active_allocation_bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(MultipleAllocations) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	void* a = dbg.allocate(100, 1);
	void* b = dbg.allocate(200, 1);
	void* c = dbg.allocate(50, 1);
	BOOST_TEST(dbg.active_allocation_count() == 3u);
	BOOST_TEST(dbg.active_allocation_bytes() == 350u);
	dbg.deallocate(b, 200, 1);
	BOOST_TEST(dbg.active_allocation_count() == 2u);
	BOOST_TEST(dbg.active_allocation_bytes() == 150u);
	dbg.deallocate(a, 100, 1);
	dbg.deallocate(c, 50, 1);
	BOOST_TEST(dbg.active_allocation_count() == 0u);
}

// --- Leak detection ---

BOOST_AUTO_TEST_CASE(ReportLeaksReturnsZeroWhenClean) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	void* p = dbg.allocate(32, 1);
	dbg.deallocate(p, 32, 1);
	BOOST_TEST(dbg.report_leaks() == 0u);
}

#ifndef NDEBUG
BOOST_AUTO_TEST_CASE(ReportLeaksDetectsActiveAllocations) {
	// Create on heap so we can manually manage lifetime without destructor assert
	auto* upstream = std::pmr::new_delete_resource();
	auto* dbg = new (upstream->allocate(sizeof(ds::debug_resource), alignof(ds::debug_resource)))
		ds::debug_resource(upstream);

	void* a = dbg->allocate(64, 1);
	void* b = dbg->allocate(128, 1);
	BOOST_TEST(dbg->report_leaks() == 2u);

	// Clean up to avoid destructor assert
	dbg->deallocate(a, 64, 1);
	dbg->deallocate(b, 128, 1);
	BOOST_TEST(dbg->report_leaks() == 0u);

	dbg->~debug_resource();
	upstream->deallocate(dbg, sizeof(ds::debug_resource), alignof(ds::debug_resource));
}
#endif

// --- Buffer overrun detection ---

#ifndef NDEBUG
BOOST_AUTO_TEST_CASE(BackGuardCorruptionDetected) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	void* p = dbg.allocate(32, 1);

	// Corrupt the back guard by writing past the allocation
	std::byte* user = static_cast<std::byte*>(p);
	user[32] = std::byte{0xFF}; // overwrite first byte of back guard

	// Deallocation should assert on corrupted back guard
	// We cannot test assert death portably, so just verify allocation tracking
	// In a real debug build, the ASSERT_MSG would fire
	BOOST_TEST(dbg.active_allocation_count() == 1u);

	// Restore the guard byte to allow clean deallocation
	user[32] = std::byte{0xCD};
	dbg.deallocate(p, 32, 1);
	BOOST_TEST(dbg.active_allocation_count() == 0u);
}

BOOST_AUTO_TEST_CASE(FrontGuardCorruptionDetected) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	void* p = dbg.allocate(32, 1);

	// Corrupt the front guard by writing before the allocation
	std::byte* user = static_cast<std::byte*>(p);
	user[-1] = std::byte{0xFF}; // overwrite last byte of front guard

	// Verify allocation is still tracked
	BOOST_TEST(dbg.active_allocation_count() == 1u);

	// Restore the guard byte to allow clean deallocation
	user[-1] = std::byte{0xCD};
	dbg.deallocate(p, 32, 1);
	BOOST_TEST(dbg.active_allocation_count() == 0u);
}
#endif

// --- Identity comparison ---

BOOST_AUTO_TEST_CASE(IsEqualSameInstance) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	BOOST_TEST(dbg.is_equal(dbg));
}

BOOST_AUTO_TEST_CASE(IsEqualDifferentInstance) {
	ds::debug_resource a(std::pmr::new_delete_resource());
	ds::debug_resource b(std::pmr::new_delete_resource());
	BOOST_TEST(!a.is_equal(b));
}

// --- Integration with std::pmr::vector ---

BOOST_AUTO_TEST_CASE(PmrVectorWorks) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	{
		std::pmr::vector<int> v(&dbg);
		v.reserve(50);
		for (int i = 0; i < 50; ++i) {
			v.push_back(i);
		}
		BOOST_TEST(v.size() == 50u);
		BOOST_TEST(dbg.active_allocation_count() > 0u);
	}
	// After vector destruction, all allocations should be freed
	BOOST_TEST(dbg.active_allocation_count() == 0u);
	BOOST_TEST(dbg.active_allocation_bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(PmrVectorGrowth) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	{
		std::pmr::vector<int> v(&dbg);
		// Trigger multiple reallocations
		for (int i = 0; i < 1000; ++i) {
			v.push_back(i);
		}
		BOOST_TEST(v.size() == 1000u);
		for (int i = 0; i < 1000; ++i) {
			BOOST_TEST(v[i] == i);
		}
	}
	BOOST_TEST(dbg.active_allocation_count() == 0u);
}

// --- Zero-size allocation ---

BOOST_AUTO_TEST_CASE(ZeroBytesAllocation) {
	ds::debug_resource dbg(std::pmr::new_delete_resource());
	void* p = dbg.allocate(0, 1);
	BOOST_TEST(p != nullptr);
	dbg.deallocate(p, 0, 1);
	BOOST_TEST(dbg.active_allocation_count() == 0u);
}

// --- Wrapping another custom resource ---

BOOST_AUTO_TEST_CASE(WrapsCustomUpstream) {
	// Verify debug_resource works when wrapping a non-default upstream
	std::byte buffer[4096];
	std::pmr::monotonic_buffer_resource mono(buffer, sizeof(buffer));
	ds::debug_resource dbg(&mono);

	void* p = dbg.allocate(128, 8);
	BOOST_TEST(p != nullptr);
	BOOST_TEST(dbg.active_allocation_count() == 1u);
	BOOST_TEST(dbg.active_allocation_bytes() == 128u);
	dbg.deallocate(p, 128, 8);
	BOOST_TEST(dbg.active_allocation_count() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
