#include <snk/platform/plt_file_lock.h>
#include <snk/common/fs_paths.h>
#include <snk/platform/plt_link_check.h>
#include <format>
#include <optional>
#include <string>
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

#if defined(_WIN32)
	std::optional<std::wstring> final_path(HANDLE handle)
	{
		constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
		const DWORD size = ::GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
		if (size == 0) {
			return std::nullopt;
		}
		std::wstring path(size, L'\0');
		const DWORD length = ::GetFinalPathNameByHandleW(handle, path.data(), size, flags);
		if (length == 0 || length >= size) {
			return std::nullopt;
		}
		path.resize(length);
		return path;
	}

	// The opened file is the root's (as it resolves now) followed by `parts`: no folder between was swapped for a
	// junction after it was checked (the same check as platform::beneath on Windows).
	bool lies_below_root(HANDLE file, const fs::path& root, const std::vector<fs::path>& parts)
	{
		// Both sides are final paths (the \\?\ or \\?\UNC\ form), so the comparison does not depend on how `root`
		// is spelled.
		const fs::path long_root = platform::beneath::long_path(root);
		const HANDLE root_handle = ::CreateFileW(long_root.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (root_handle == INVALID_HANDLE_VALUE) {
			return false;
		}
		std::optional<std::wstring> expected = final_path(root_handle);
		::CloseHandle(root_handle);
		const std::optional<std::wstring> actual = final_path(file);
		if (!expected || !actual) {
			return false;
		}
		for (const fs::path& part : parts) {
			expected->append(L"\\").append(part.native());
		}
		return ::CompareStringOrdinal(actual->c_str(), static_cast<int>(actual->size()), expected->c_str(),
			static_cast<int>(expected->size()), TRUE) == CSTR_EQUAL;
	}
#endif
}

namespace
{
	platform::lock_attempt refused(platform::lock_outcome outcome, std::string error = {})
	{
		platform::lock_attempt out;
		out.outcome = outcome;
		out.error = std::move(error);
		return out;
	}

#if !defined(_WIN32)
	// Separate tests: EAGAIN and EWOULDBLOCK (ENOTSUP and EOPNOTSUPP) are one value on some systems.
	bool lock_is_held(int code)
	{
		if (code == EWOULDBLOCK) {
			return true;
		}
		return code == EAGAIN;
	}

	bool lock_is_unsupported(int code)
	{
		if (code == ENOLCK || code == EOPNOTSUPP) {
			return true;
		}
		return code == ENOTSUP;
	}
#endif
}

// One attempt; `gone` is set when the file was locked but no longer lies at its name (removed or replaced meanwhile).
platform::lock_attempt platform::file_lock::acquire_once(const fs::path& root, const std::vector<fs::path>& parts, bool create,
	bool& gone)
{
	gone = false;
	const fs::path where = [&] {
		fs::path path = root;
		for (const fs::path& part : parts) {
			path /= part;
		}
		return path;
	}();
#if defined(_WIN32)
	const auto is_missing = [](DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; };
	// Checked one folder at a time, then the file is opened without following a reparse point and checked through its
	// handle; its final path then shows that no folder was swapped for a junction after its check.
	// Every path below in its \\?\ form, so a root past MAX_PATH works (#1352); messages name the plain path.
	if (create) {
		std::error_code made;
		fs::create_directories(platform::beneath::long_path(root), made);
	}
	fs::path folder = root;
	for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
		folder /= parts[i];
		const fs::path long_folder = platform::beneath::long_path(folder);
		DWORD attributes = ::GetFileAttributesW(long_folder.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			const DWORD code = ::GetLastError();
			if (!create) {
				return is_missing(code) ? refused(lock_outcome::missing)
										: refused(lock_outcome::failed, std::format("cannot inspect '{}': {}", fs_paths::utf8_of(folder),
											  std::system_category().message(static_cast<int>(code))));
			}
			if (!::CreateDirectoryW(long_folder.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
				return refused(lock_outcome::failed, std::format("cannot make '{}': {}", fs_paths::utf8_of(folder),
					std::system_category().message(static_cast<int>(::GetLastError()))));
			}
			attributes = ::GetFileAttributesW(long_folder.c_str());
		}
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			return refused(lock_outcome::failed, std::format("'{}' is a link or not a folder", fs_paths::utf8_of(folder)));
		}
	}
	const fs::path long_where = platform::beneath::long_path(where);
	const HANDLE handle = ::CreateFileW(long_where.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		const DWORD code = ::GetLastError();
		if (!create && is_missing(code)) {
			return refused(lock_outcome::missing);
		}
		return refused(lock_outcome::failed, std::format("cannot open '{}': {}", fs_paths::utf8_of(where),
			std::system_category().message(static_cast<int>(code))));
	}
	BY_HANDLE_FILE_INFORMATION information{};
	if (!::GetFileInformationByHandle(handle, &information)
		|| (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0
		|| !lies_below_root(handle, root, parts)) {
		::CloseHandle(handle);
		return refused(lock_outcome::failed, std::format("'{}' is a link or not a file", fs_paths::utf8_of(where)));
	}
	OVERLAPPED whole{};
	if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &whole)) {
		const DWORD code = ::GetLastError();
		::CloseHandle(handle);
		if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
			return refused(lock_outcome::held);
		}
		const std::string text = std::format("cannot lock '{}': {}", fs_paths::utf8_of(where),
			std::system_category().message(static_cast<int>(code)));
		const bool unsupported = code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION;
		return refused(unsupported ? lock_outcome::unsupported : lock_outcome::failed, text);
	}
	file_lock held{ reinterpret_cast<std::intptr_t>(handle) };
	FILE_STANDARD_INFO standard{};
	if (!::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) || standard.DeletePending
		|| standard.NumberOfLinks == 0) {
		gone = true;
		return refused(lock_outcome::missing);
	}
#else
	constexpr int k_folder_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
	int at = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (at < 0 && errno == ENOENT && create) {
		std::error_code ec;
		fs::create_directories(root, ec);
		at = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	}
	if (at < 0) {
		if (!create && errno == ENOENT) {
			return refused(lock_outcome::missing);
		}
		return refused(lock_outcome::failed, std::format("cannot open '{}': {}", fs_paths::utf8_of(root), std::generic_category().message(errno)));
	}
	fs::path reached = root;
	for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
		const fs::path& name = parts[i];
		reached /= name;
		int next = ::openat(at, name.c_str(), k_folder_flags);
		if (next < 0 && errno == ENOENT && create) {
			if (::mkdirat(at, name.c_str(), 0777) == 0 || errno == EEXIST) {
				next = ::openat(at, name.c_str(), k_folder_flags);
			}
		}
		const int code = errno;
		::close(at);
		if (next < 0) {
			if (!create && code == ENOENT) {
				return refused(lock_outcome::missing);
			}
			return refused(lock_outcome::failed, std::format("cannot open the folder '{}' (a link is not followed): {}",
				fs_paths::utf8_of(reached), std::generic_category().message(code)));
		}
		at = next;
	}
	// O_NONBLOCK: a FIFO at the name cannot hold the editor; it is refused below as not a regular file.
	const int open_flags = O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (create ? O_CREAT : 0);
	const int fd = ::openat(at, parts.back().c_str(), open_flags, 0644);
	if (fd < 0) {
		const int code = errno;
		::close(at);
		if (!create && code == ENOENT) {
			return refused(lock_outcome::missing);
		}
		return refused(lock_outcome::failed, std::format("cannot open '{}' (a link is not followed): {}", fs_paths::utf8_of(where),
			std::generic_category().message(code)));
	}
	struct stat information{};
	if (::fstat(fd, &information) != 0 || !S_ISREG(information.st_mode)) {
		::close(fd);
		::close(at);
		return refused(lock_outcome::failed, std::format("'{}' is not a regular file", fs_paths::utf8_of(where)));
	}
	while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		const int code = errno;
		if (code == EINTR) {
			continue;
		}
		::close(fd);
		::close(at);
		if (lock_is_held(code)) {
			return refused(lock_outcome::held);
		}
		const std::string text = std::format("cannot lock '{}': {}", fs_paths::utf8_of(where), std::generic_category().message(code));
		return refused(lock_is_unsupported(code) ? lock_outcome::unsupported : lock_outcome::failed, text);
	}
	file_lock held{ static_cast<std::intptr_t>(fd) };
	struct stat named{};
	const bool still_there = ::fstatat(at, parts.back().c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0
		&& named.st_ino == information.st_ino && named.st_dev == information.st_dev;
	::close(at);
	if (!still_there) {
		gone = true;
		return refused(lock_outcome::missing);
	}
#endif
	lock_attempt out;
	out.outcome = lock_outcome::acquired;
	out.lock.emplace(std::move(held));
	return out;
}

platform::lock_attempt platform::file_lock::acquire(const fs::path& root, const fs::path& relative, lock_open open)
{
	const std::optional<std::vector<fs::path>> parts = plain_parts(relative);
	if (!parts) {
		return refused(lock_outcome::failed, std::format("'{}' is not a chain of plain names", fs_paths::utf8_of(relative)));
	}
	const bool create = open == lock_open::create;
	// A file removed between its open and its lock (its holder cleaning up) is made again once or twice; a probe
	// reports it missing.
	for (int attempt = 0;; ++attempt) {
		bool gone = false;
		lock_attempt result = acquire_once(root, *parts, create, gone);
		if (!gone || !create || attempt == 2) {
			return result;
		}
	}
}

std::optional<platform::file_lock> platform::file_lock::try_acquire(const fs::path& root, const fs::path& relative, std::string* error)
{
	lock_attempt attempt = acquire(root, relative, lock_open::create);
	if (attempt.outcome == lock_outcome::acquired) {
		return std::move(attempt.lock);
	}
	if (attempt.outcome != lock_outcome::held) {
		set_error(error, attempt.error.empty() ? std::string{ "the lock file was removed while it was taken" } : std::move(attempt.error));
	}
	return std::nullopt;
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
