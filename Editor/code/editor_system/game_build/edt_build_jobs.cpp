#include "edt_build_jobs.h"
#include <snk/platform/plt_memory.h>
#include <algorithm>
#include <thread>

int edt::recommended_jobs(unsigned cores, std::uint64_t ram_bytes, compiler_family compiler)
{
	const std::uint64_t core_count = std::max(cores, 1u);
	if (ram_bytes == 0) {
		return static_cast<int>(core_count);
	}
	if (ram_bytes <= k_jobs_ram_reserve) {
		return 1;
	}
	const std::uint64_t per_job = compiler == compiler_family::msvc ? k_ram_per_job_msvc : k_ram_per_job_gnu;
	const std::uint64_t by_ram = (ram_bytes - k_jobs_ram_reserve) / per_job;
	return static_cast<int>(std::clamp<std::uint64_t>(std::min(core_count, by_ram), 1, core_count));
}

edt::compiler_family edt::host_compiler_family()
{
#ifdef _MSC_VER
	return compiler_family::msvc;
#else
	return compiler_family::gnu;
#endif
}

edt::jobs_estimate edt::estimate_jobs()
{
	jobs_estimate estimate;
	estimate.cores = std::thread::hardware_concurrency();
	estimate.ram = platform::total_physical_memory();
	estimate.compiler = host_compiler_family();
	estimate.jobs = recommended_jobs(estimate.cores, estimate.ram, estimate.compiler);
	return estimate;
}
