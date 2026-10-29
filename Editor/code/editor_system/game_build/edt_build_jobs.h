#pragma once
#include <cstdint>

namespace edt
{
	// How much memory one compile job of the game module takes: MSVC needs more than GCC/Clang.
	enum class compiler_family { msvc, gnu };

	inline constexpr std::uint64_t k_gib = 1024ull * 1024ull * 1024ull;
	// Left to the system, the editor and the linker.
	inline constexpr std::uint64_t k_jobs_ram_reserve = 2 * k_gib;
	inline constexpr std::uint64_t k_ram_per_job_msvc = 2 * k_gib;
	inline constexpr std::uint64_t k_ram_per_job_gnu = 3 * k_gib / 2;

	// clamp(min(cores, (ram - reserve) / per_job), 1, cores). `cores` 0 counts as 1; `ram_bytes` 0 (unknown) gives
	// `cores`; RAM at or under the reserve gives 1.
	int recommended_jobs(unsigned cores, std::uint64_t ram_bytes, compiler_family compiler);

	// The compiler the editor itself was built with: the game module is built with the same one.
	compiler_family host_compiler_family();

	// The recommendation and what it is based on, for the UI ("16 cores, 32 GiB -> 15").
	struct jobs_estimate
	{
		int jobs = 1;
		unsigned cores = 0;
		std::uint64_t ram = 0;
		compiler_family compiler = compiler_family::gnu;
	};

	// recommended_jobs for this machine (std::thread::hardware_concurrency, platform::total_physical_memory).
	jobs_estimate estimate_jobs();
}
