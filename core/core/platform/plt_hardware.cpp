#include <snk/platform/plt_hardware.h>
#include <algorithm>
#include <charconv>
#include <system_error>
#include <thread>

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

#ifdef __linux__
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sched.h>
#endif

namespace
{
	std::string_view trim(std::string_view text)
	{
		const auto first = text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}
		return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
	}

	std::optional<std::uint64_t> parse_number(std::string_view text)
	{
		std::uint64_t value = 0;
		const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
			return std::nullopt;
		}
		return value;
	}

	std::uint64_t host_physical_memory()
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

#ifdef __linux__
	namespace fs = std::filesystem;

	const fs::path k_cgroup_root = "/sys/fs/cgroup";

	std::optional<std::string> read_small_file(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			return std::nullopt;
		}
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	// The process's own cgroup and its ancestors: a limit set on a parent (a systemd slice) applies too. Inside a
	// container without its own cgroup namespace that path is not mounted, and the root (the container's) is used.
	std::vector<fs::path> cgroup_v2_dirs()
	{
		std::vector<fs::path> dirs;
		if (const auto self = read_small_file("/proc/self/cgroup")) {
			if (const auto path = platform::detail::parse_cgroup_v2_path(*self)) {
				const fs::path relative = fs::path(std::string(*path)).relative_path();
				fs::path dir = k_cgroup_root / relative;
				std::error_code ec;
				if (!relative.empty() && fs::is_directory(dir, ec)) {
					for (; dir != k_cgroup_root && dir.has_relative_path(); dir = dir.parent_path()) {
						dirs.push_back(dir);
					}
				}
			}
		}
		dirs.push_back(k_cgroup_root);
		return dirs;
	}

	template <class T, class Parse>
	std::optional<T> lowest_limit(const std::vector<fs::path>& dirs, const char* file_name, Parse parse)
	{
		std::optional<T> lowest;
		for (const auto& dir : dirs) {
			if (const auto text = read_small_file(dir / file_name)) {
				if (const auto limit = parse(*text)) {
					lowest = lowest ? std::min<T>(*lowest, *limit) : *limit;
				}
			}
		}
		return lowest;
	}
#endif
}

std::optional<std::uint64_t> platform::detail::parse_cgroup_memory_max(std::string_view text)
{
	return parse_number(trim(text));
}

std::optional<unsigned> platform::detail::parse_cgroup_cpu_max(std::string_view text)
{
	const std::string_view line = trim(text);
	const auto space = line.find(' ');
	if (space == std::string_view::npos) {
		return std::nullopt;
	}
	const auto quota = parse_number(line.substr(0, space));
	const auto period = parse_number(trim(line.substr(space + 1)));
	if (!quota || !period || *quota == 0 || *period == 0) {
		return std::nullopt;
	}
	const std::uint64_t cpus = (*quota + *period - 1) / *period;
	return static_cast<unsigned>(std::clamp<std::uint64_t>(cpus, 1, 0xFFFFFFFFu));
}

std::optional<std::string_view> platform::detail::parse_cgroup_v2_path(std::string_view proc_self_cgroup)
{
	constexpr std::string_view k_v2_prefix = "0::";
	while (!proc_self_cgroup.empty()) {
		const auto end = proc_self_cgroup.find('\n');
		const std::string_view line = trim(proc_self_cgroup.substr(0, end));
		if (line.starts_with(k_v2_prefix) && line.size() > k_v2_prefix.size()) {
			return line.substr(k_v2_prefix.size());
		}
		if (end == std::string_view::npos) {
			break;
		}
		proc_self_cgroup.remove_prefix(end + 1);
	}
	return std::nullopt;
}

std::uint64_t platform::total_physical_memory()
{
	std::uint64_t memory = host_physical_memory();
#ifdef __linux__
	const auto lower_to = [&memory](std::optional<std::uint64_t> limit) {
		// cgroup v1 reports "no limit" as a huge number, above the installed RAM.
		if (limit && *limit > 0 && (memory == 0 || *limit < memory)) {
			memory = *limit;
		}
	};
	lower_to(lowest_limit<std::uint64_t>(cgroup_v2_dirs(), "memory.max", detail::parse_cgroup_memory_max));
	if (const auto v1 = read_small_file(k_cgroup_root / "memory" / "memory.limit_in_bytes")) {
		lower_to(detail::parse_cgroup_memory_max(*v1));
	}
#endif
	return memory;
}

unsigned platform::available_cpu_count()
{
	unsigned cpus = std::thread::hardware_concurrency();
#ifdef __linux__
	cpu_set_t set;
	CPU_ZERO(&set);
	if (::sched_getaffinity(0, sizeof(set), &set) == 0) {
		cpus = static_cast<unsigned>(CPU_COUNT(&set));
	}
	if (const auto quota = lowest_limit<unsigned>(cgroup_v2_dirs(), "cpu.max", detail::parse_cgroup_cpu_max)) {
		cpus = cpus == 0 ? *quota : std::min(cpus, *quota);
	}
#endif
	return cpus;
}
