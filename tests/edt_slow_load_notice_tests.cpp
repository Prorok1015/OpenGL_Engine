#include <boost/test/unit_test.hpp>
#include "edt_slow_load_notice.h"
#include <chrono>

// #613: a level load that runs long is told about once per period, on the caller's clock.

BOOST_AUTO_TEST_SUITE(EdtSlowLoadNotice)

BOOST_AUTO_TEST_CASE(ALongLoad_IsToldOncePerPeriod_WithHowLongItRan)
{
	using namespace std::chrono_literals;
	const auto t0 = edt::slow_load_notice::clock::time_point{} + 1000s;
	edt::slow_load_notice notice{ 30s };
	notice.start(t0);

	BOOST_TEST(!notice.due(t0).has_value());
	BOOST_TEST(!notice.due(t0 + 29s).has_value());
	const auto first = notice.due(t0 + 30s);
	BOOST_REQUIRE(first.has_value());
	BOOST_TEST(first->count() == 30);
	BOOST_TEST(!notice.due(t0 + 31s).has_value());
	BOOST_TEST(!notice.due(t0 + 59s).has_value());
	const auto second = notice.due(t0 + 75s);
	BOOST_REQUIRE(second.has_value());
	BOOST_TEST(second->count() == 75);
	BOOST_TEST(!notice.due(t0 + 90s).has_value());

	// A new load starts its own count.
	notice.start(t0 + 200s);
	BOOST_TEST(!notice.due(t0 + 210s).has_value());
	BOOST_TEST(notice.due(t0 + 230s).value().count() == 30);
}

BOOST_AUTO_TEST_SUITE_END()
