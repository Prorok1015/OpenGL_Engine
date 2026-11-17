#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <snk/common/snk_api.h>

namespace platform
{
	// The RAM this process may use, in bytes: the installed RAM, on Linux lowered to the cgroup memory limit (a
	// container's); 0 when the system cannot tell.
	SNK_API std::uint64_t total_physical_memory();

	// The CPUs this process may run on: on Linux its affinity mask, lowered to the cgroup v2 CPU quota; elsewhere
	// std::thread::hardware_concurrency(). 0 when the system cannot tell.
	SNK_API unsigned available_cpu_count();

	namespace detail
	{
		// cgroup v2 memory.max or v1 memory.limit_in_bytes: the limit in bytes; nullopt for "max" (no limit) or text
		// that is not a number.
		SNK_API std::optional<std::uint64_t> parse_cgroup_memory_max(std::string_view text);
		// cgroup v2 cpu.max ("<quota> <period>", quota "max" = no limit): ceil(quota / period), at least 1; nullopt
		// for no limit or text that is not that.
		SNK_API std::optional<unsigned> parse_cgroup_cpu_max(std::string_view text);
		// The cgroup v2 path of the process from /proc/self/cgroup (the "0::<path>" line); nullopt without one.
		SNK_API std::optional<std::string_view> parse_cgroup_v2_path(std::string_view proc_self_cgroup);
	}
}
