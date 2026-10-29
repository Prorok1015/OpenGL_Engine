#include <snk/platform/plt_memory.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

std::uint64_t platform::total_physical_memory()
{
#ifdef _WIN32
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status)) {
		return 0;
	}
	return static_cast<std::uint64_t>(status.ullTotalPhys);
#else
	const long pages = ::sysconf(_SC_PHYS_PAGES);
	const long page_size = ::sysconf(_SC_PAGE_SIZE);
	if (pages <= 0 || page_size <= 0) {
		return 0;
	}
	return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
#endif
}
