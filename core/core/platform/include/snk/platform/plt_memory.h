#pragma once
#include <cstdint>
#include <snk/common/snk_api.h>

namespace platform
{
	// The installed RAM in bytes; 0 when the system cannot tell.
	SNK_API std::uint64_t total_physical_memory();
}
