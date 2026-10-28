#include "res_vfs_write.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <system_error>
#include <thread>
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
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

	// Components of a path below the root, without "." (lexically_relative gives "." for the root itself).
	std::vector<fs::path> folder_parts(const fs::path& folder)
	{
		std::vector<fs::path> parts;
		for (const auto& part : folder) {
			if (!part.empty() && part != ".") {
				parts.push_back(part);
			}
		}
		return parts;
	}

}

#if !defined(_WIN32)

namespace {

	std::string errno_text(std::string_view what, const fs::path& path)
	{
		return std::format("{} '{}': {}", what, path.string(), std::strerror(errno));
	}

	class fd_holder
	{
	public:
		explicit fd_holder(int fd = -1) : m_fd(fd) {}
		~fd_holder() { reset(); }
		fd_holder(fd_holder&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
		fd_holder& operator=(fd_holder&& other) noexcept
		{
			if (this != &other) {
				reset();
				m_fd = std::exchange(other.m_fd, -1);
			}
			return *this;
		}
		fd_holder(const fd_holder&) = delete;
		fd_holder& operator=(const fd_holder&) = delete;

		int get() const { return m_fd; }
		int release() { return std::exchange(m_fd, -1); }
		void reset()
		{
			if (m_fd >= 0) {
				::close(m_fd);
				m_fd = -1;
			}
		}

	private:
		int m_fd;
	};

	constexpr int k_dir_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

	// The folder `name` in `parent`, never through a link; made first when `create` and it is missing.
	int open_folder(int parent, const fs::path& name, bool create)
	{
		int fd = ::openat(parent, name.c_str(), k_dir_flags);
		if (fd < 0 && errno == ENOENT && create) {
			if (::mkdirat(parent, name.c_str(), 0755) != 0 && errno != EEXIST) {
				return -1;
			}
			fd = ::openat(parent, name.c_str(), k_dir_flags);
		}
		return fd;
	}

	// The folder below the root, one component at a time. -1 with errno set when one is a link, not a folder, or
	// (without `create`) missing.
	fd_holder open_below(int root, const fs::path& folder, bool create, fs::path& reached)
	{
		fd_holder current;
		int at = root;
		for (const auto& part : folder_parts(folder)) {
			reached /= part;
			fd_holder next{ open_folder(at, part, create) };
			if (next.get() < 0) {
				const int error = errno; // closing `current` must not change what the caller reports
				current.reset();
				errno = error;
				return fd_holder{};
			}
			current = std::move(next);
			at = current.get();
		}
		if (current.get() < 0) {
			// The folder is the root itself: a descriptor of its own, so the caller may close it.
			return fd_holder{ ::fcntl(root, F_DUPFD_CLOEXEC, 0) };
		}
		return current;
	}

	bool write_all(int fd, std::span<const std::byte> data)
	{
		std::size_t done = 0;
		while (done < data.size()) {
			const ssize_t wrote = ::write(fd, data.data() + done, data.size() - done);
			if (wrote < 0) {
				if (errno == EINTR) {
					continue;
				}
				return false;
			}
			done += static_cast<std::size_t>(wrote);
		}
		return true;
	}

}

res::detail::pinned_root::~pinned_root()
{
	if (m_fd >= 0) {
		::close(m_fd);
	}
}

int res::detail::pinned_root::descriptor(std::string& error)
{
	if (m_fd >= 0) {
		struct stat information;
		// The folder it named was removed (a fresh project made later): open the one there now.
		if (::fstat(m_fd, &information) != 0 || information.st_nlink == 0) {
			::close(m_fd);
			m_fd = -1;
		}
	}
	if (m_fd < 0) {
		// The root is the mounted folder itself, pinned canonical when mounted: made by path if it does not exist yet.
		std::error_code ec;
		fs::create_directories(m_root, ec);
		m_fd = ::open(m_root.c_str(), k_dir_flags);
		if (m_fd < 0) {
			error = errno_text("cannot open the resource root", m_root);
		}
	}
	return m_fd;
}

std::string res::detail::write_beneath(pinned_root& root, const write_request& request)
{
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path reached = root.path();
	const fd_holder folder = open_below(root_fd, request.folder, true, reached);
	if (folder.get() < 0) {
		return errno_text("cannot open the folder (a link, or not a folder)", reached);
	}

	// O_EXCL with O_NOFOLLOW: nothing planted at the temp name is opened, not even a link.
	fd_holder file{ ::openat(folder.get(), request.tmp_leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644) };
	if (file.get() < 0) {
		return errno_text("cannot create", request.tmp_path);
	}
	const auto drop_tmp = [&](std::string why) {
		::unlinkat(folder.get(), request.tmp_leaf.c_str(), 0);
		return why;
	};
	if (!write_all(file.get(), request.data)) {
		return drop_tmp(errno_text("writing failed for", request.tmp_path));
	}
	if (request.durable && ::fsync(file.get()) != 0) {
		return drop_tmp(errno_text("cannot flush", request.tmp_path));
	}
	if (::close(file.release()) != 0) {
		return drop_tmp(errno_text("writing failed for", request.tmp_path));
	}

	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::before_rename, request.tmp_path);
	}
	// Inside the folder that was opened: a link at the target's name is replaced itself, never followed.
	if (::renameat(folder.get(), request.tmp_leaf.c_str(), folder.get(), request.leaf.c_str()) != 0) {
		return drop_tmp(errno_text("cannot replace", reached / request.leaf));
	}
	if (request.durable) {
		::fsync(folder.get());
	}
	return {};
}

std::string res::detail::remove_beneath(pinned_root& root, const fs::path& folder, std::string_view leaf)
{
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path reached = root.path();
	const fd_holder at = open_below(root_fd, folder, false, reached);
	if (at.get() < 0) {
		return errno == ENOENT ? std::string{} : errno_text("cannot open the folder (a link, or not a folder)", reached);
	}
	const std::string name{ leaf };
	if (::unlinkat(at.get(), name.c_str(), 0) != 0 && errno != ENOENT) {
		return errno_text("cannot remove", reached / name);
	}
	return {};
}

#else

namespace {

	std::string last_error_text(std::string_view what, const fs::path& path)
	{
		const DWORD code = ::GetLastError();
		return std::format("{} '{}': {}", what, path.string(), std::system_category().message(static_cast<int>(code)));
	}

	class handle_holder
	{
	public:
		explicit handle_holder(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
		~handle_holder() { reset(); }
		handle_holder(handle_holder&& other) noexcept : m_handle(std::exchange(other.m_handle, INVALID_HANDLE_VALUE)) {}
		handle_holder& operator=(handle_holder&& other) noexcept
		{
			if (this != &other) {
				reset();
				m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
			}
			return *this;
		}
		handle_holder(const handle_holder&) = delete;
		handle_holder& operator=(const handle_holder&) = delete;

		HANDLE get() const { return m_handle; }
		bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
		void reset()
		{
			if (valid()) {
				::CloseHandle(m_handle);
				m_handle = INVALID_HANDLE_VALUE;
			}
		}

	private:
		HANDLE m_handle;
	};

	// A real folder (not a reparse point), held without FILE_SHARE_DELETE: while the handle is open nobody can rename,
	// remove or replace it, so the names below it keep resolving through it.
	handle_holder hold_folder(const fs::path& path, bool create)
	{
		const auto open = [&] {
			return handle_holder{ ::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
		};
		handle_holder folder = open();
		if (!folder.valid() && create && (::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND)) {
			if (!::CreateDirectoryW(path.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
				return handle_holder{};
			}
			folder = open();
		}
		if (!folder.valid()) {
			return folder;
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(folder.get(), &information)
			|| (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
			|| (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return handle_holder{};
		}
		return folder;
	}

	// The root and every folder down to `folder`, all held until the chain goes; empty on failure, `reached` naming
	// where it stopped.
	std::vector<handle_holder> hold_chain(const fs::path& root, const fs::path& folder, bool create, fs::path& reached)
	{
		std::vector<handle_holder> chain;
		// Closing what was held must not change the error the caller reports.
		const auto give_up = [&chain] {
			const DWORD code = ::GetLastError();
			chain.clear();
			::SetLastError(code);
			return std::vector<handle_holder>{};
		};
		if (create) {
			std::error_code ec;
			fs::create_directories(root, ec); // the mounted folder itself, pinned when mounted
		}
		reached = root;
		handle_holder top = hold_folder(reached, false);
		if (!top.valid()) {
			return give_up();
		}
		chain.push_back(std::move(top));
		for (const auto& part : folder_parts(folder)) {
			reached /= part;
			handle_holder next = hold_folder(reached, create);
			if (!next.valid()) {
				return give_up();
			}
			chain.push_back(std::move(next));
		}
		return chain;
	}

	bool set_delete_on_close(HANDLE file)
	{
		FILE_DISPOSITION_INFO disposition{};
		disposition.DeleteFile = TRUE;
		return ::SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
	}

	// Renames the open file to `target` (a path through the held folders), replacing what is there; a link at the
	// target is replaced itself.
	bool rename_to(HANDLE file, const fs::path& target)
	{
		const std::wstring name = target.native();
		std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
		auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
		info->ReplaceIfExists = TRUE;
		info->RootDirectory = nullptr;
		info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
		std::memcpy(info->FileName, name.data(), name.size() * sizeof(wchar_t));
		return ::SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(buffer.size())) != 0;
	}

}

res::detail::pinned_root::~pinned_root() = default;

std::string res::detail::write_beneath(pinned_root& root, const write_request& request)
{
	fs::path reached;
	const std::vector<handle_holder> chain = hold_chain(root.path(), request.folder, true, reached);
	if (chain.empty()) {
		return last_error_text("cannot open the folder (a reparse point, or not a folder)", reached);
	}
	const fs::path tmp = reached / request.tmp_leaf;
	const fs::path target = reached / request.leaf;
	// CREATE_NEW: nothing planted at the temp name is opened, not even a link.
	handle_holder file{ ::CreateFileW(tmp.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
	if (!file.valid()) {
		return last_error_text("cannot create", tmp);
	}
	const auto drop_tmp = [&](std::string why) {
		set_delete_on_close(file.get());
		file.reset();
		return why;
	};
	std::size_t done = 0;
	while (done < request.data.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(request.data.size() - done, 1u << 30));
		DWORD wrote = 0;
		if (!::WriteFile(file.get(), request.data.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
			return drop_tmp(last_error_text("writing failed for", tmp));
		}
		done += wrote;
	}
	if (request.durable && !::FlushFileBuffers(file.get())) {
		return drop_tmp(last_error_text("cannot flush", tmp));
	}

	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::before_rename, request.tmp_path);
	}
	// A file that was just removed (an erase on this thread) can stay delete-pending while another handle (watcher,
	// indexer, antivirus) is open on it, and replacing it is denied for a moment.
	bool renamed = false;
	for (int attempt = 0; attempt < 50 && !renamed; ++attempt) {
		renamed = rename_to(file.get(), target);
		if (!renamed) {
			if (::GetLastError() != ERROR_ACCESS_DENIED) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	if (!renamed) {
		return drop_tmp(last_error_text("cannot replace", target));
	}
	return {};
}

std::string res::detail::remove_beneath(pinned_root& root, const fs::path& folder, std::string_view leaf)
{
	fs::path reached;
	const std::vector<handle_holder> chain = hold_chain(root.path(), folder, false, reached);
	if (chain.empty()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
			? std::string{}
			: last_error_text("cannot open the folder (a reparse point, or not a folder)", reached);
	}
	const fs::path path = reached / fs::path{ std::string{ leaf } };
	handle_holder file{ ::CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
	if (!file.valid()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? std::string{} : last_error_text("cannot remove", path);
	}
	if (!set_delete_on_close(file.get())) {
		return last_error_text("cannot remove", path);
	}
	return {};
}

#endif
