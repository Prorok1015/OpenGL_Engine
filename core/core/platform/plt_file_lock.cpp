#include <snk/platform/plt_file_lock.h>
#include <snk/common/fs_paths.h>
#include <format>
#include <system_error>
#include <utility>
#include <vector>
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
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
	// The plain names of `relative`; none when it holds anything else (a root, "..", ".", an empty name).
	std::optional<std::vector<fs::path>> plain_parts(const fs::path& relative)
	{
		std::vector<fs::path> parts;
		for (const fs::path& part : relative) {
			const fs::path::string_type& text = part.native();
			if (text.empty() || part == "." || part == ".." || part.has_root_name() || part.has_root_directory()) {
				return std::nullopt;
			}
			parts.push_back(part);
		}
		if (parts.empty()) {
			return std::nullopt;
		}
		return parts;
	}

	void set_error(std::string* error, std::string text)
	{
		if (error) {
			*error = std::move(text);
		}
	}
}

std::optional<platform::file_lock> platform::file_lock::try_acquire(const fs::path& root, const fs::path& relative, std::string* error)
{
	const std::optional<std::vector<fs::path>> parts = plain_parts(relative);
	if (!parts) {
		set_error(error, std::format("'{}' is not a chain of plain names", fs_paths::utf8_of(relative)));
		return std::nullopt;
	}
	const fs::path where = root / relative;
#if defined(_WIN32)
	// Checked one folder at a time, then the file is opened without following a reparse point and checked through its
	// handle. A folder swapped for a junction between the check and the open is not caught: the project's own content
	// is what this guards against, not a process racing the editor.
	std::error_code made;
	fs::create_directories(root, made);
	fs::path folder = root;
	for (std::size_t i = 0; i + 1 < parts->size(); ++i) {
		folder /= (*parts)[i];
		DWORD attributes = ::GetFileAttributesW(folder.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			if (!::CreateDirectoryW(folder.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
				set_error(error, std::format("cannot make '{}': {}", fs_paths::utf8_of(folder), std::system_category().message(static_cast<int>(::GetLastError()))));
				return std::nullopt;
			}
			attributes = ::GetFileAttributesW(folder.c_str());
		}
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			set_error(error, std::format("'{}' is a link or not a folder", fs_paths::utf8_of(folder)));
			return std::nullopt;
		}
	}
	const HANDLE handle = ::CreateFileW(where.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		set_error(error, std::format("cannot open '{}': {}", fs_paths::utf8_of(where), std::system_category().message(static_cast<int>(::GetLastError()))));
		return std::nullopt;
	}
	BY_HANDLE_FILE_INFORMATION information{};
	if (!::GetFileInformationByHandle(handle, &information)
		|| (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
		::CloseHandle(handle);
		set_error(error, std::format("'{}' is a link or not a file", fs_paths::utf8_of(where)));
		return std::nullopt;
	}
	OVERLAPPED whole{};
	if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &whole)) {
		::CloseHandle(handle);
		return std::nullopt;
	}
	return file_lock{ reinterpret_cast<std::intptr_t>(handle) };
#else
	constexpr int k_folder_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
	int at = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (at < 0 && errno == ENOENT) {
		std::error_code ec;
		fs::create_directories(root, ec);
		at = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	}
	if (at < 0) {
		set_error(error, std::format("cannot open '{}': {}", fs_paths::utf8_of(root), std::generic_category().message(errno)));
		return std::nullopt;
	}
	fs::path reached = root;
	for (std::size_t i = 0; i + 1 < parts->size(); ++i) {
		const fs::path& name = (*parts)[i];
		reached /= name;
		int next = ::openat(at, name.c_str(), k_folder_flags);
		if (next < 0 && errno == ENOENT) {
			if (::mkdirat(at, name.c_str(), 0777) == 0 || errno == EEXIST) {
				next = ::openat(at, name.c_str(), k_folder_flags);
			}
		}
		const int code = errno;
		::close(at);
		if (next < 0) {
			set_error(error, std::format("cannot open the folder '{}' (a link is not followed): {}", fs_paths::utf8_of(reached),
				std::generic_category().message(code)));
			return std::nullopt;
		}
		at = next;
	}
	// O_NONBLOCK: a FIFO at the name cannot hold the editor; it is refused below as not a regular file.
	const int fd = ::openat(at, parts->back().c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
	const int code = errno;
	::close(at);
	if (fd < 0) {
		set_error(error, std::format("cannot open '{}' (a link is not followed): {}", fs_paths::utf8_of(where), std::generic_category().message(code)));
		return std::nullopt;
	}
	struct stat information{};
	if (::fstat(fd, &information) != 0 || !S_ISREG(information.st_mode)) {
		::close(fd);
		set_error(error, std::format("'{}' is not a regular file", fs_paths::utf8_of(where)));
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
