#include <boost/test/unit_test.hpp>
#include "ecs_random.hpp"
#include <set>
#include <vector>

BOOST_AUTO_TEST_SUITE(EcsRandomTests)

BOOST_AUTO_TEST_CASE(MatchesThePcg32ReferenceVector)
{
	// pcg32 demo: srandom(42, 54)
	auto r = ecs::rng::from_state(42u, 54u);
	const uint32_t expected[] = { 0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu };
	for (uint32_t e : expected) BOOST_TEST(r.next_u32() == e);
}

BOOST_AUTO_TEST_CASE(SameSeed_GivesTheSameSequence)
{
	ecs::rng a(1234), b(1234);
	for (int i = 0; i < 1000; ++i) BOOST_TEST(a.next_u32() == b.next_u32());
}

BOOST_AUTO_TEST_CASE(DifferentSeeds_GiveDifferentSequences)
{
	ecs::rng a(1), b(2);
	int equal = 0;
	for (int i = 0; i < 100; ++i) equal += a.next_u32() == b.next_u32();
	BOOST_TEST(equal < 3);
}

BOOST_AUTO_TEST_CASE(Reseed_RestartsTheSequence)
{
	ecs::rng r(77);
	std::vector<uint32_t> first;
	for (int i = 0; i < 10; ++i) first.push_back(r.next_u32());
	r.reseed(77);
	for (uint32_t v : first) BOOST_TEST(r.next_u32() == v);
	BOOST_TEST(r.seed() == 77u);
}

BOOST_AUTO_TEST_CASE(Copy_ContinuesIndependently)
{
	ecs::rng a(5);
	(void)a.next_u32();
	ecs::rng b = a;
	BOOST_TEST(a.next_u32() == b.next_u32());
}

BOOST_AUTO_TEST_CASE(Ranges_AreRespected)
{
	ecs::rng r(9);
	std::set<int> seen;
	for (int i = 0; i < 2000; ++i) {
		const float f = r.next_float();
		BOOST_REQUIRE(f >= 0.0f && f < 1.0f);
		const float g = r.next_float(-2.0f, 3.0f);
		BOOST_REQUIRE(g >= -2.0f && g < 3.0f);
		const int n = r.next_int(-3, 3);
		BOOST_REQUIRE(n >= -3 && n <= 3);
		seen.insert(n);
		BOOST_REQUIRE(r.next_below(7) < 7u);
	}
	BOOST_TEST(seen.size() == 7u); // every value of the inclusive range shows up
	BOOST_TEST(r.next_below(0) == 0u);
	BOOST_TEST(r.next_int(4, 4) == 4);
}

BOOST_AUTO_TEST_CASE(DeriveSeed_SeparatesStreams)
{
	BOOST_TEST(ecs::derive_seed(1, 0) != ecs::derive_seed(1, 1));
	BOOST_TEST(ecs::derive_seed(1, 0) != ecs::derive_seed(2, 0));
	BOOST_TEST(ecs::derive_seed(1, 5) == ecs::derive_seed(1, 5));
}

BOOST_AUTO_TEST_SUITE_END()
