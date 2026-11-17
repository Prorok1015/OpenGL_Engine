#include <boost/test/unit_test.hpp>
#include "edt_build_jobs.h"
#include <cstdint>

// The recommended number of parallel compile jobs for the game build (#444).

using edt::compiler_family;
using edt::k_gib;
using edt::recommended_jobs;

BOOST_AUTO_TEST_SUITE(EdtBuildJobsTests)

BOOST_AUTO_TEST_CASE(RecommendedJobs_Table)
{
	struct row
	{
		unsigned cores;
		std::uint64_t ram;
		compiler_family compiler;
		int jobs;
	};
	const row rows[] = {
		{ 16, 32 * k_gib, compiler_family::msvc, 15 },
		{ 16, 32 * k_gib, compiler_family::gnu, 16 },
		{ 8, 8 * k_gib, compiler_family::msvc, 3 },
		{ 8, 8 * k_gib, compiler_family::gnu, 4 },
	};
	for (const auto& r : rows) {
		BOOST_TEST_INFO("cores " << r.cores << ", GiB " << r.ram / k_gib << ", msvc " << (r.compiler == compiler_family::msvc));
		BOOST_TEST(recommended_jobs(r.cores, r.ram, r.compiler) == r.jobs);
	}
}

BOOST_AUTO_TEST_CASE(RecommendedJobs_NoCores_CountsAsOne)
{
	BOOST_TEST(recommended_jobs(0, 64 * k_gib, compiler_family::gnu) == 1);
	BOOST_TEST(recommended_jobs(0, 0, compiler_family::msvc) == 1);
}

BOOST_AUTO_TEST_CASE(RecommendedJobs_UnknownRam_UsesAllCores)
{
	BOOST_TEST(recommended_jobs(12, 0, compiler_family::msvc) == 12);
}

BOOST_AUTO_TEST_CASE(RecommendedJobs_RamAtOrUnderTheReserve_IsOne)
{
	BOOST_TEST(recommended_jobs(8, edt::k_jobs_ram_reserve, compiler_family::gnu) == 1);
	BOOST_TEST(recommended_jobs(8, k_gib, compiler_family::msvc) == 1);
	// Above the reserve but short of one job's worth.
	BOOST_TEST(recommended_jobs(8, edt::k_jobs_ram_reserve + k_gib, compiler_family::msvc) == 1);
}

BOOST_AUTO_TEST_CASE(RecommendedJobs_PlentyOfRam_IsCappedByCores)
{
	BOOST_TEST(recommended_jobs(4, 256 * k_gib, compiler_family::msvc) == 4);
}

BOOST_AUTO_TEST_CASE(EstimateJobs_IsWithinTheCores)
{
	const auto estimate = edt::estimate_jobs();
	BOOST_TEST(estimate.jobs >= 1);
	BOOST_TEST(static_cast<unsigned>(estimate.jobs) <= (estimate.cores == 0 ? 1u : estimate.cores));
	BOOST_TEST((estimate.compiler == edt::host_compiler_family()));
}

BOOST_AUTO_TEST_SUITE_END()
