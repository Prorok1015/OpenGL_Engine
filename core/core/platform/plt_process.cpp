#include <snk/platform/plt_process.h>
#include <cstddef>
#include <format>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

std::wstring platform::quote_windows_arg(std::wstring_view arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring{ arg };
	}
	std::wstring out = L"\"";
	for (auto it = arg.begin();; ++it) {
		std::size_t backslashes = 0;
		while (it != arg.end() && *it == L'\\') {
			++it;
			++backslashes;
		}
		if (it == arg.end()) {
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
		} else {
			out.append(backslashes, L'\\');
		}
		out.push_back(*it);
	}
	out.push_back(L'"');
	return out;
}

#ifdef _WIN32

namespace
{
	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::wstring widen(const std::string& utf8)
	{
		if (utf8.empty()) {
			return {};
		}
		const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
		return out;
	}
}

fs::path platform::current_executable()
{
	std::wstring buffer(MAX_PATH, L'\0');
	// Long paths can exceed MAX_PATH; the documented limit of a Windows path is 32767 characters.
	while (buffer.size() <= 32768) {
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0) {
			return {};
		}
		if (length < buffer.size()) {
			buffer.resize(length);
			return fs::path{ buffer };
		}
		buffer.resize(buffer.size() * 2);
	}
	return {};
}

platform::spawn_result platform::spawn_detached(const fs::path& program, const std::vector<std::string>& args)
{
	std::wstring command_line = quote_windows_arg(program.native());
	for (const auto& arg : args) {
		command_line += L' ';
		command_line += quote_windows_arg(widen(arg));
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	// CreateProcessW may write into the command line, so it gets its own buffer.
	if (!CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, FALSE,
			DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process)) {
		const auto error = std::error_code{ static_cast<int>(GetLastError()), std::system_category() };
		return { 0, std::format("cannot start '{}': {}", utf8_of(program), error.message()) };
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return { process.dwProcessId, {} };
}

#else

fs::path platform::current_executable()
{
	std::error_code ec;
	const fs::path path = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::path{} : path;
}

platform::spawn_result platform::spawn_detached(const fs::path& program, const std::vector<std::string>& args)
{
	std::string program_name = program.string();
	std::vector<std::string> owned = args;
	std::vector<char*> argv;
	argv.reserve(owned.size() + 2);
	argv.push_back(program_name.data());
	for (auto& arg : owned) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);

	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
	// Its own session: closing the terminal (or the process group) of this editor does not take the new one along.
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
	// Only stdin/out/err go along: Asio does not open its sockets close-on-exec, so the new editor would otherwise
	// hold the listening socket of this one (and its log files) for as long as it runs.
	posix_spawn_file_actions_addclosefrom_np(&file_actions, 3);
#endif
#endif
	pid_t pid = 0;
	const int result = posix_spawn(&pid, program_name.c_str(), &file_actions, &attributes, argv.data(), environ);
	posix_spawn_file_actions_destroy(&file_actions);
	posix_spawnattr_destroy(&attributes);
	if (result != 0) {
		return { 0, std::format("cannot start '{}': {}", program_name, std::generic_category().message(result)) };
	}
	return { static_cast<std::uint64_t>(pid), {} };
}

#endif
