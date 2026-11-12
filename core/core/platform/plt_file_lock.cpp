#include <snk/platform/plt_file_lock.h>
#include <snk/common/fs_paths.h>
#include <format>
#include <system_error>
#include <utility>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

std::optional<platform::file_lock> platform::file_lock::try_acquire(const fs::path& path, std::string* error)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
#if defined(_WIN32)
	const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		if (error) {
			*error = std::format("cannot open '{}': {}", fs_paths::utf8_of(path), std::system_category().message(static_cast<int>(::GetLastError())));
		}
		return std::nullopt;
	}
	OVERLAPPED whole{};
	if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &whole)) {
		::CloseHandle(handle);
		return std::nullopt;
	}
	return file_lock{ reinterpret_cast<std::intptr_t>(handle) };
#else
	const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		if (error) {
			*error = std::format("cannot open '{}': {}", fs_paths::utf8_of(path), std::generic_category().message(errno));
		}
		return std::nullopt;
	}
	while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		if (errno == EINTR) {
			continue;
		}
		::close(fd);
		return std::nullopt;
	}
	return file_lock{ static_cast<std::intptr_t>(fd) };
#endif
}

platform::file_lock::file_lock(file_lock&& other) noexcept : m_handle(std::exchange(other.m_handle, -1)) {}

platform::file_lock& platform::file_lock::operator=(file_lock&& other) noexcept
{
	if (this != &other) {
		release();
		m_handle = std::exchange(other.m_handle, -1);
	}
	return *this;
}

platform::file_lock::~file_lock()
{
	release();
}

void platform::file_lock::release() noexcept
{
	if (m_handle == -1) {
		return;
	}
#if defined(_WIN32)
	const HANDLE handle = reinterpret_cast<HANDLE>(m_handle);
	OVERLAPPED whole{};
	::UnlockFileEx(handle, 0, 1, 0, &whole);
	::CloseHandle(handle);
#else
	const int fd = static_cast<int>(m_handle);
	::flock(fd, LOCK_UN);
	::close(fd);
#endif
	m_handle = -1;
}
