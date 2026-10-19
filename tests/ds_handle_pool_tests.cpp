#include <boost/test/unit_test.hpp>
#include <snk/common/ds/ds_handle_pool.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

BOOST_AUTO_TEST_SUITE(DSHandlePoolTests)

BOOST_AUTO_TEST_CASE(InsertedValueIsFoundByItsHandle)
{
	ds::handle_pool<std::string> pool;
	const auto h = pool.insert("alpha");

	BOOST_TEST(static_cast<bool>(h));
	BOOST_TEST(h.index == 1u);
	BOOST_REQUIRE(pool.find(h));
	BOOST_TEST(*pool.find(h) == "alpha");
	BOOST_TEST(pool.live() == 1u);
	BOOST_TEST(pool.capacity() == 1u);
}

BOOST_AUTO_TEST_CASE(NullAndUnknownHandlesFindNothing)
{
	ds::handle_pool<int> pool;
	pool.insert(7);

	BOOST_TEST((pool.find(ds::pool_handle{}) == nullptr));
	BOOST_TEST((pool.find(ds::pool_handle{ 99, 1 }) == nullptr));
	BOOST_TEST(!pool.stale(ds::pool_handle{}));
	BOOST_TEST(!pool.stale(ds::pool_handle{ 99, 1 }));
	BOOST_TEST(!pool.erase(ds::pool_handle{}));
	BOOST_TEST(!pool.erase(ds::pool_handle{ 99, 1 }));
	BOOST_TEST(pool.live() == 1u);
}

BOOST_AUTO_TEST_CASE(ErasedHandleIsStale)
{
	ds::handle_pool<int> pool;
	const auto h = pool.insert(1);

	BOOST_TEST(!pool.stale(h));
	BOOST_TEST(pool.erase(h));
	BOOST_TEST((pool.find(h) == nullptr));
	BOOST_TEST(pool.stale(h));
	BOOST_TEST(pool.live() == 0u);
	// Erasing twice is refused, the pool is not corrupted.
	BOOST_TEST(!pool.erase(h));
	BOOST_TEST(pool.live() == 0u);
}

BOOST_AUTO_TEST_CASE(FreedSlotIsReusedWithTheNextGeneration)
{
	ds::handle_pool<int> pool;
	const auto first = pool.insert(10);
	pool.erase(first);
	const auto second = pool.insert(20);

	BOOST_TEST(second.index == first.index);
	BOOST_TEST(second.gen == first.gen + 1);
	BOOST_TEST((second != first));
	BOOST_TEST(pool.capacity() == 1u);
	// The old handle does not name the new value.
	BOOST_TEST((pool.find(first) == nullptr));
	BOOST_TEST(pool.stale(first));
	BOOST_REQUIRE(pool.find(second));
	BOOST_TEST(*pool.find(second) == 20);
}

BOOST_AUTO_TEST_CASE(FreeListHandsOutTheLastFreedSlotFirst)
{
	ds::handle_pool<int> pool;
	const auto a = pool.insert(1);
	const auto b = pool.insert(2);
	const auto c = pool.insert(3);
	pool.erase(a);
	pool.erase(c);

	BOOST_TEST(pool.insert(4).index == c.index);
	BOOST_TEST(pool.insert(5).index == a.index);
	BOOST_TEST(pool.insert(6).index == 4u);
	BOOST_TEST(*pool.find(b) == 2);
	BOOST_TEST(pool.live() == 4u);
	BOOST_TEST(pool.capacity() == 4u);
}

BOOST_AUTO_TEST_CASE(GenerationKeepsCountingAcrossManyReuses)
{
	ds::handle_pool<int> pool;
	auto h = pool.insert(0);
	for (int i = 1; i <= 100; ++i) {
		const auto old = h;
		pool.erase(h);
		h = pool.insert(i);
		BOOST_TEST(h.index == old.index);
		BOOST_TEST(h.gen == old.gen + 1);
		BOOST_TEST(pool.stale(old));
	}
	BOOST_TEST(*pool.find(h) == 100);
	BOOST_TEST(pool.capacity() == 1u);
}

BOOST_AUTO_TEST_CASE(PoolHoldsMoveOnlyValues)
{
	ds::handle_pool<std::unique_ptr<int>> pool;
	const auto h = pool.insert(std::make_unique<int>(42));

	BOOST_REQUIRE(pool.find(h));
	BOOST_TEST(**pool.find(h) == 42);
	pool.erase(h);
	BOOST_TEST((pool.find(h) == nullptr));
}

BOOST_AUTO_TEST_CASE(ErasingDestroysTheValue)
{
	const auto alive = std::make_shared<int>(0);
	ds::handle_pool<std::shared_ptr<int>> pool;
	const auto h = pool.insert(alive);
	BOOST_TEST(alive.use_count() == 2);
	pool.erase(h);
	BOOST_TEST(alive.use_count() == 1);
}

BOOST_AUTO_TEST_CASE(ForEachVisitsLiveValuesWithTheirHandles)
{
	ds::handle_pool<int> pool;
	const auto a = pool.insert(1);
	const auto b = pool.insert(2);
	const auto c = pool.insert(3);
	pool.erase(b);

	std::vector<std::pair<ds::pool_handle, int>> seen;
	pool.for_each([&](ds::pool_handle h, int& value) { seen.emplace_back(h, value); });

	BOOST_REQUIRE(seen.size() == 2u);
	BOOST_TEST((seen[0].first == a));
	BOOST_TEST(seen[0].second == 1);
	BOOST_TEST((seen[1].first == c));
	BOOST_TEST(seen[1].second == 3);
}

namespace {
	// A handle type of its own, as the render drivers have: the pool only needs `index` and `gen`.
	struct my_handle
	{
		std::uint32_t index = 0;
		std::uint32_t gen = 0;
	};
}

BOOST_AUTO_TEST_CASE(PoolBuildsTheHandleTypeItIsGiven)
{
	ds::handle_pool<std::string, my_handle> pool;
	const my_handle h = pool.insert("beta");

	BOOST_TEST(h.index == 1u);
	BOOST_TEST(h.gen == 1u);
	BOOST_REQUIRE(pool.find(h));
	BOOST_TEST(*pool.find(h) == "beta");
	pool.erase(h);
	BOOST_TEST(pool.stale(h));
}

BOOST_AUTO_TEST_SUITE_END()
