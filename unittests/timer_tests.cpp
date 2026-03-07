#include <boost/test/unit_test.hpp>
#include "timer.hpp"

BOOST_AUTO_TEST_SUITE(TimerTests)

BOOST_AUTO_TEST_CASE(Timer_ElapsedTime)
{
    double startTime = Timer::now();
    // Simulate some work
    for (int i = 0; i < 1000000; ++i) {
        // do nothing
    }
    double endTime = Timer::now();
    BOOST_TEST(endTime - startTime > 0.0);
}

BOOST_AUTO_TEST_SUITE_END()
