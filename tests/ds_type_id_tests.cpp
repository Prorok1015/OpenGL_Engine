#include <boost/test/unit_test.hpp>
#include "ds/ds_type_id.hpp"
#include <array>
#include <atomic>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	template<int N>
	struct probe_type {};

	constexpr int k_type_count = 256;
	constexpr int k_thread_count = 8;

	template<int... Is>
	std::array<ds::type_id (*)(), sizeof...(Is)> make_getters(std::integer_sequence<int, Is...>)
	{
		return { +[]() { return ds::type_id::make<probe_type<Is>>(); }... };
	}
}

BOOST_AUTO_TEST_SUITE(DSTypeIdTests)

// Regression guard for the startup crash (#83): type ids are first requested concurrently by the main
// thread and the resource adapter worker. A non-atomic counter handed the same id to two types, which
// made the resource system pick the wrong adapter / storage slot for a type.
BOOST_AUTO_TEST_CASE(ConcurrentFirstUseYieldsUniqueIds)
{
	const auto getters = make_getters(std::make_integer_sequence<int, k_type_count>{});

	std::vector<std::array<ds::type_id, k_type_count>> seen(k_thread_count);
	std::atomic<bool> go{ false };
	std::vector<std::thread> threads;
	threads.reserve(k_thread_count);
	for (int t = 0; t < k_thread_count; ++t) {
		threads.emplace_back([&, t] {
			while (!go.load(std::memory_order_acquire)) {}
			for (int i = 0; i < k_type_count; ++i) {
				const int idx = (i + t * 31) % k_type_count;
				seen[t][idx] = getters[idx]();
			}
		});
	}
	go.store(true, std::memory_order_release);
	for (auto& th : threads) th.join();

	std::set<ds::type_id::unique_id> unique;
	for (int i = 0; i < k_type_count; ++i) {
		for (int t = 1; t < k_thread_count; ++t) {
			BOOST_REQUIRE(seen[t][i] == seen[0][i]);
		}
		unique.insert(seen[0][i].id);
	}
	BOOST_CHECK_EQUAL(unique.size(), static_cast<std::size_t>(k_type_count));
}

BOOST_AUTO_TEST_SUITE_END()
