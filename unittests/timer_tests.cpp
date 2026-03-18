#include <boost/test/unit_test.hpp>
#include "timer.hpp"
#include <thread>

BOOST_AUTO_TEST_SUITE(TimerTests)

BOOST_AUTO_TEST_CASE(Now_ReturnsPositiveValue) {
	double t = Timer::now();
	BOOST_TEST(t > 0.0);
}

BOOST_AUTO_TEST_CASE(Now_IsMonotonicallyIncreasing) {
	double t1 = Timer::now();
	double t2 = Timer::now();
	BOOST_TEST(t2 >= t1);
}

BOOST_AUTO_TEST_CASE(Now_MeasuresElapsedTime) {
	double t1 = Timer::now();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	double t2 = Timer::now();
	double elapsed = t2 - t1;
	BOOST_TEST(elapsed >= 0.04);
	BOOST_TEST(elapsed < 1.0);
}

BOOST_AUTO_TEST_CASE(NowSec_ReturnsPositiveValue) {
	double t = Timer::now_sec();
	BOOST_TEST(t > 0.0);
}

BOOST_AUTO_TEST_CASE(NowSec_IsMonotonicallyIncreasing) {
	double t1 = Timer::now_sec();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	double t2 = Timer::now_sec();
	BOOST_TEST(t2 >= t1);
}

BOOST_AUTO_TEST_SUITE_END()
