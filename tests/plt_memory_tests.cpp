#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_memory.h>

BOOST_AUTO_TEST_SUITE(PltMemoryTests)

BOOST_AUTO_TEST_CASE(TotalPhysicalMemory_IsKnown)
{
	BOOST_TEST(platform::total_physical_memory() > 0u);
}

BOOST_AUTO_TEST_SUITE_END()
