#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_hardware.h>
#include <cstdint>
#include <optional>
#include <string_view>

using platform::detail::parse_cgroup_cpu_max;
using platform::detail::parse_cgroup_memory_max;
using platform::detail::parse_cgroup_v2_path;

BOOST_AUTO_TEST_SUITE(PltHardwareTests)

BOOST_AUTO_TEST_CASE(TotalPhysicalMemory_IsKnownAndInBytes)
{
	BOOST_TEST(platform::total_physical_memory() >= 256ull * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(AvailableCpuCount_IsKnown)
{
	BOOST_TEST(platform::available_cpu_count() >= 1u);
}

BOOST_AUTO_TEST_CASE(CgroupMemoryMax_Parses)
{
	BOOST_TEST((parse_cgroup_memory_max("4294967296\n") == std::optional<std::uint64_t>{ 4294967296ull }));
	BOOST_TEST(!parse_cgroup_memory_max("max\n").has_value());
	BOOST_TEST(!parse_cgroup_memory_max("").has_value());
	BOOST_TEST(!parse_cgroup_memory_max("12abc").has_value());
	BOOST_TEST(!parse_cgroup_memory_max("-5").has_value());
}

BOOST_AUTO_TEST_CASE(CgroupCpuMax_Parses)
{
	BOOST_TEST((parse_cgroup_cpu_max("200000 100000\n") == std::optional<unsigned>{ 2u }));
	BOOST_TEST((parse_cgroup_cpu_max("150000 100000") == std::optional<unsigned>{ 2u }));
	BOOST_TEST((parse_cgroup_cpu_max("1000 100000") == std::optional<unsigned>{ 1u }));
	BOOST_TEST(!parse_cgroup_cpu_max("max 100000\n").has_value());
	BOOST_TEST(!parse_cgroup_cpu_max("garbage").has_value());
	BOOST_TEST(!parse_cgroup_cpu_max("100 0").has_value());
	BOOST_TEST(!parse_cgroup_cpu_max("").has_value());
}

BOOST_AUTO_TEST_CASE(CgroupV2Path_IsTheZeroLine)
{
	const auto path = parse_cgroup_v2_path("12:memory:/old\n0::/user.slice/session-1.scope\n");
	BOOST_REQUIRE(path.has_value());
	BOOST_TEST(*path == "/user.slice/session-1.scope");
	BOOST_TEST(!parse_cgroup_v2_path("4:cpu:/x\n").has_value());
	BOOST_TEST(!parse_cgroup_v2_path("").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
