#include <snk/platform/plt_shell_open.h>
#include <snk/platform/plt_process.h>
#include <snk/common/fs_paths.h>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

	std::string refusal_of(const fs::path& file)
	{
		if (!file.is_absolute()) {
			return std::format("'{}' is not an absolute path", fs_paths::utf8_of(file));
		}
		std::error_code ec;
		if (!fs::is_regular_file(file, ec)) {
			return std::format("'{}' is not an existing file", fs_paths::utf8_of(file));
		}
		return {};
	}

#ifndef _WIN32
	fs::path program_for_opening()
	{
#if defined(__APPLE__)
		return "/usr/bin/open";
#else
		const char* path_value = std::getenv("PATH");
		std::string rest = path_value ? path_value : "";
		std::size_t begin = 0;
		while (begin <= rest.size()) {
			const std::size_t end = rest.find(':', begin);
			const std::string dir = rest.substr(begin, end == std::string::npos ? end : end - begin);
			if (!dir.empty()) {
				const fs::path candidate = fs::path{ dir } / "xdg-open";
				struct stat info{};
				if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(candidate.c_str(), X_OK) == 0) {
					return candidate;
				}
			}
			if (end == std::string::npos) break;
			begin = end + 1;
		}
		return {};
#endif
	}
#endif

}

std::string platform::open_in_shell(const fs::path& file)
{
	if (std::string refusal = refusal_of(file); !refusal.empty()) {
		return refusal;
	}
#ifdef _WIN32
	SHELLEXECUTEINFOW info{};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	info.lpVerb = L"open";
	info.lpFile = file.c_str();
	info.nShow = SW_SHOWNORMAL;
	if (!::ShellExecuteExW(&info)) {
		const DWORD code = ::GetLastError();
		if (code == ERROR_NO_ASSOCIATION) {
			return std::format("no program is associated with '{}'", fs_paths::utf8_of(file));
		}
		return std::format("cannot open '{}': {}", fs_paths::utf8_of(file), std::system_category().message(static_cast<int>(code)));
	}
	return {};
#else
	const fs::path program = program_for_opening();
	if (program.empty()) {
		return "xdg-open was not found in PATH";
	}
	const auto started = platform::spawn_detached(program, { fs_paths::utf8_of(file) });
	return started.error;
#endif
}
