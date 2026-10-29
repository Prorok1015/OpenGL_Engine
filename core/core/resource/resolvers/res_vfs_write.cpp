#include "res_vfs_write.h"
#include <snk/common/logger/engine_log.h>
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
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
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <stdio.h>
#endif
#endif

namespace fs = std::filesystem;

namespace {

	// The folders below the root. safe_path never yields an absolute path or "..", but the walk does not rely on it.
	std::optional<std::vector<fs::path>> folder_parts(const fs::path& folder)
	{
		if (folder.has_root_path()) {
			return std::nullopt;
		}
		std::vector<fs::path> parts;
		for (const auto& part : folder) {
			if (part == "..") {
				return std::nullopt;
			}
			// lexically_relative gives "." for the root itself; a trailing separator gives an empty one.
			if (!part.empty() && part != ".") {
				parts.push_back(part);
			}
		}
		return parts;
	}

	bool is_plain_name(const fs::path& name)
	{
		return !name.empty() && !name.has_root_path() && name == name.filename() && name != "." && name != "..";
	}

	constexpr std::string_view k_not_below = "the path is not a chain of folder names below the resource root";

}

#if !defined(_WIN32)

namespace {

	std::string errno_text(std::string_view what, const fs::path& path)
	{
		return std::format("{} '{}': {}", what, fs_paths::utf8_of(path), std::generic_category().message(errno));
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

	// Removes the temp file from its folder unless the write got through; also when the write throws.
	class temp_guard
	{
	public:
		temp_guard(int folder, const fs::path& name) : m_folder(folder), m_name(name) {}
		~temp_guard()
		{
			if (m_armed) {
				const int error = errno; // the caller's message is built from it
				::unlinkat(m_folder, m_name.c_str(), 0);
				errno = error;
			}
		}
		temp_guard(const temp_guard&) = delete;
		temp_guard& operator=(const temp_guard&) = delete;

		void dismiss() { m_armed = false; }

	private:
		int m_folder;
		const fs::path& m_name;
		bool m_armed = true;
	};

	constexpr int k_dir_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

	// The folder `name` in `parent`, never through a link; made first when `create` and it is missing.
	int open_folder(int parent, const fs::path& name, bool create)
	{
		int fd = ::openat(parent, name.c_str(), k_dir_flags);
		if (fd < 0 && errno == ENOENT && create) {
			// The umask applies, as it did to the folders std::filesystem made.
			if (::mkdirat(parent, name.c_str(), 0777) != 0 && errno != EEXIST) {
				return -1;
			}
			fd = ::openat(parent, name.c_str(), k_dir_flags);
		}
		return fd;
	}

	// The folder below the root, one component at a time. -1 with errno set when one is a link, not a folder, or
	// (without `create`) missing.
	fd_holder open_below(int root, const std::vector<fs::path>& parts, bool create, fs::path& reached)
	{
		fd_holder current;
		int at = root;
		for (const auto& part : parts) {
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

	// Moves the entry without replacing one at the target. Linux has it atomically (renameat2, RENAME_NOREPLACE = 1);
	// elsewhere, or on a file system without the flag, the target is looked up first inside the opened folder.
	int rename_no_replace(int from_folder, const char* from, int to_folder, const char* to)
	{
#if defined(__linux__) && defined(SYS_renameat2)
		if (::syscall(SYS_renameat2, from_folder, from, to_folder, to, 1u) == 0) {
			return 0;
		}
		if (errno != ENOSYS && errno != EINVAL) {
			return -1;
		}
#endif
#if defined(__APPLE__) && defined(RENAME_EXCL)
		if (::renameatx_np(from_folder, from, to_folder, to, RENAME_EXCL) == 0) {
			return 0;
		}
		if (errno != ENOTSUP && errno != EINVAL) {
			return -1;
		}
#endif
		struct stat existing;
		if (::fstatat(to_folder, to, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
			errno = EEXIST;
			return -1;
		}
		if (errno != ENOENT) {
			return -1;
		}
		return ::renameat(from_folder, from, to_folder, to);
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
		struct stat held;
		struct stat at_path;
		// The folder at the root's path is no longer the one held (removed, or renamed and replaced): open the one there.
		if (::fstat(m_fd, &held) != 0 || held.st_nlink == 0 || ::lstat(m_root.c_str(), &at_path) != 0
			|| held.st_dev != at_path.st_dev || held.st_ino != at_path.st_ino) {
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
	const auto parts = folder_parts(request.folder);
	if (!parts || !is_plain_name(request.leaf) || !is_plain_name(request.tmp_leaf)) {
		return std::string{ k_not_below };
	}
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path reached = root.path();
	const fd_holder folder = open_below(root_fd, *parts, true, reached);
	if (folder.get() < 0) {
		return errno_text("cannot open the folder (a link, or not a folder)", reached);
	}
	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::folders_held, request.tmp_path);
	}

	// O_EXCL with O_NOFOLLOW: nothing planted at the temp name is opened, not even a link. The umask applies.
	fd_holder file{ ::openat(folder.get(), request.tmp_leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666) };
	if (file.get() < 0) {
		return errno_text("cannot create", request.tmp_path);
	}
	temp_guard guard{ folder.get(), request.tmp_leaf };
	if (!write_all(file.get(), request.data)) {
		return errno_text("writing failed for", request.tmp_path);
	}
	if (request.durable && ::fsync(file.get()) != 0) {
		return errno_text("cannot flush", request.tmp_path);
	}
	if (::close(file.release()) != 0) {
		return errno_text("writing failed for", request.tmp_path);
	}

	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::before_rename, request.tmp_path);
	}
	// Inside the folder that was opened: a link at the target's name is replaced itself, never followed.
	const int moved = request.replace
		? ::renameat(folder.get(), request.tmp_leaf.c_str(), folder.get(), request.leaf.c_str())
		: rename_no_replace(folder.get(), request.tmp_leaf.c_str(), folder.get(), request.leaf.c_str());
	if (moved != 0) {
		return errno_text(request.replace ? "cannot replace" : "cannot create", reached / request.leaf);
	}
	guard.dismiss();
	// The file is in place: a folder that was not flushed only weakens durability across a crash.
	if (request.durable && ::fsync(folder.get()) != 0) {
		egLOG_WARN("resource/write", "{}", errno_text("cannot flush the folder", reached));
	}
	return {};
}

std::string res::detail::remove_beneath(pinned_root& root, const fs::path& folder, const fs::path& leaf, bool files_only)
{
	const auto parts = folder_parts(folder);
	if (!parts || !is_plain_name(leaf)) {
		return std::string{ k_not_below };
	}
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path reached = root.path();
	const fd_holder at = open_below(root_fd, *parts, false, reached);
	if (at.get() < 0) {
		return errno == ENOENT ? std::string{} : errno_text("cannot open the folder (a link, or not a folder)", reached);
	}
	if (::unlinkat(at.get(), leaf.c_str(), 0) == 0) {
		return {};
	}
	// A folder: removed when empty, as std::filesystem::remove did (Linux reports EISDIR, POSIX allows EPERM).
	int code = errno;
	if (!files_only && (code == EISDIR || code == EPERM)) {
		if (::unlinkat(at.get(), leaf.c_str(), AT_REMOVEDIR) == 0) {
			return {};
		}
		if (errno != ENOTDIR) {
			code = errno;
		}
	}
	if (code == ENOENT) {
		return {};
	}
	errno = code;
	return errno_text("cannot remove", reached / leaf);
}

std::string res::detail::rename_beneath(pinned_root& root, const rename_request& request)
{
	const auto from_parts = folder_parts(request.from_folder);
	const auto to_parts = folder_parts(request.to_folder);
	if (!from_parts || !to_parts || !is_plain_name(request.from_leaf) || !is_plain_name(request.to_leaf)) {
		return std::string{ k_not_below };
	}
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path from_reached = root.path();
	const fd_holder from_folder = open_below(root_fd, *from_parts, false, from_reached);
	if (from_folder.get() < 0) {
		return errno_text("cannot open the folder (a link, or not a folder)", from_reached);
	}

	// The source first: a rename that cannot happen touches nothing.
	struct stat source;
	if (::fstatat(from_folder.get(), request.from_leaf.c_str(), &source, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno_text("cannot find", from_reached / request.from_leaf);
	}
	if (!S_ISREG(source.st_mode)) {
		return std::format("'{}' is not a regular file", fs_paths::utf8_of(from_reached / request.from_leaf));
	}
	fs::path to_reached = root.path();
	const fd_holder to_folder = open_below(root_fd, *to_parts, false, to_reached);
	if (to_folder.get() < 0) {
		return errno_text("cannot open the target folder (missing, a link, or not a folder)", to_reached);
	}
	const int moved = request.allow_replace
		? ::renameat(from_folder.get(), request.from_leaf.c_str(), to_folder.get(), request.to_leaf.c_str())
		: rename_no_replace(from_folder.get(), request.from_leaf.c_str(), to_folder.get(), request.to_leaf.c_str());
	if (moved != 0) {
		return errno_text("cannot move to", to_reached / request.to_leaf);
	}
	return {};
}

#else

#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002
#endif
#ifndef FILE_OPEN_IF
#define FILE_OPEN_IF 0x00000003
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040L
#endif

namespace {

	std::string last_error_text(std::string_view what, const fs::path& path)
	{
		const DWORD code = ::GetLastError();
		return std::format("{} '{}': {}", what, fs_paths::utf8_of(path), std::system_category().message(static_cast<int>(code)));
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

	// `name` (one component) inside the folder `parent` holds, resolved from that handle and never through a reparse
	// point: one at `name` is opened itself. Invalid with the last error set on failure.
	handle_holder open_in(HANDLE parent, const fs::path& name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options)
	{
		const std::wstring& text = name.native();
		if (text.size() * sizeof(wchar_t) > 0xfffe) {
			::SetLastError(ERROR_FILENAME_EXCED_RANGE);
			return handle_holder{};
		}
		UNICODE_STRING unicode{};
		unicode.Buffer = const_cast<PWSTR>(text.c_str());
		unicode.Length = static_cast<USHORT>(text.size() * sizeof(wchar_t));
		unicode.MaximumLength = unicode.Length;
		OBJECT_ATTRIBUTES attributes{};
		attributes.Length = sizeof(attributes);
		attributes.RootDirectory = parent;
		attributes.ObjectName = &unicode;
		attributes.Attributes = OBJ_CASE_INSENSITIVE; // as Win32 opens names
		IO_STATUS_BLOCK status_block{};
		HANDLE handle = nullptr;
		// FILE_READ_ATTRIBUTES as CreateFileW adds it: GetFileInformationByHandle and GetFinalPathNameByHandleW need it.
		const NTSTATUS status = ::NtCreateFile(&handle, access | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &attributes, &status_block, nullptr,
			FILE_ATTRIBUTE_NORMAL, share, disposition, options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, nullptr, 0);
		if (status < 0) {
			::SetLastError(::RtlNtStatusToDosError(status));
			return handle_holder{};
		}
		return handle_holder{ handle };
	}

	bool is_real_folder(HANDLE folder)
	{
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(folder, &information)) {
			return false; // its own error, not "a reparse point"
		}
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
			|| (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return false;
		}
		return true;
	}

	// Held without FILE_SHARE_DELETE: while the handle is open nobody can rename, remove or replace the folder.
	constexpr ACCESS_MASK k_folder_access = FILE_LIST_DIRECTORY | FILE_TRAVERSE;
	constexpr ULONG k_folder_share = FILE_SHARE_READ | FILE_SHARE_WRITE;

	// The root and every folder down to `parts`, each opened from the handle of the one above it and all held until
	// the chain goes; empty on failure, `reached` naming where it stopped.
	std::vector<handle_holder> hold_chain(const fs::path& root, const std::vector<fs::path>& parts, bool create, fs::path& reached)
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
		handle_holder top{ ::CreateFileW(root.c_str(), k_folder_access | SYNCHRONIZE, k_folder_share, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
		if (!top.valid() || !is_real_folder(top.get())) {
			return give_up();
		}
		chain.push_back(std::move(top));
		for (const auto& part : parts) {
			reached /= part;
			handle_holder next = open_in(chain.back().get(), part, k_folder_access, k_folder_share,
				create ? FILE_OPEN_IF : FILE_OPEN, FILE_DIRECTORY_FILE);
			if (!next.valid() || !is_real_folder(next.get())) {
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

	// Deletes the temp file when its handle closes unless the write got through; also when the write throws.
	class temp_guard
	{
	public:
		temp_guard(HANDLE file, const fs::path& path) : m_file(file), m_path(path) {}
		~temp_guard()
		{
			if (m_armed) {
				const DWORD code = ::GetLastError(); // the caller's message is built from it
				if (!set_delete_on_close(m_file)) {
					egLOG_WARN("resource/write", "{}", last_error_text("the temp file stays behind", m_path));
				}
				::SetLastError(code);
			}
		}
		temp_guard(const temp_guard&) = delete;
		temp_guard& operator=(const temp_guard&) = delete;

		void dismiss() { m_armed = false; }

	private:
		HANDLE m_file;
		const fs::path& m_path;
		bool m_armed = true;
	};

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

	// Belt and braces over the handle-relative opens: the file's final path is the root's, the folders and the name.
	bool lies_at(HANDLE file, HANDLE root, const std::vector<fs::path>& parts, const fs::path& name)
	{
		const auto actual = final_path(file);
		auto expected = final_path(root);
		if (!actual || !expected) {
			return false;
		}
		for (const auto& part : parts) {
			expected->append(L"\\").append(part.native());
		}
		expected->append(L"\\").append(name.native());
		return ::CompareStringOrdinal(actual->c_str(), static_cast<int>(actual->size()), expected->c_str(),
			static_cast<int>(expected->size()), TRUE) == CSTR_EQUAL;
	}

	// NtSetInformationFile (wdm.h), looked up rather than declared: winternl.h does not have it.
	using set_information_file_t = NTSTATUS(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, void*, ULONG, ULONG);
	constexpr ULONG k_file_rename_information = 10;

	set_information_file_t set_information_file()
	{
		static const auto function = reinterpret_cast<set_information_file_t>(
			reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile")));
		return function;
	}

	// Renames the open file to `name` inside the folder `folder` holds, replacing what is there; a link at the target
	// is replaced itself. Not SetFileInformationByHandle: it turns a relative name into a path below the current
	// directory, which together with RootDirectory is an invalid parameter.
	bool rename_to(HANDLE file, HANDLE folder, const fs::path& name, bool replace = true)
	{
		const auto set_information = set_information_file();
		if (!set_information) {
			::SetLastError(ERROR_PROC_NOT_FOUND);
			return false;
		}
		const std::wstring& text = name.native();
		// FILE_RENAME_INFORMATION has FILE_RENAME_INFO's layout.
		std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + text.size() * sizeof(wchar_t));
		auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
		info->ReplaceIfExists = replace ? TRUE : FALSE;
		info->RootDirectory = folder;
		info->FileNameLength = static_cast<DWORD>(text.size() * sizeof(wchar_t));
		std::memcpy(info->FileName, text.data(), text.size() * sizeof(wchar_t));
		IO_STATUS_BLOCK status_block{};
		const NTSTATUS status = set_information(file, &status_block, info, static_cast<ULONG>(buffer.size()), k_file_rename_information);
		if (status < 0) {
			::SetLastError(::RtlNtStatusToDosError(status));
			return false;
		}
		return true;
	}

}

res::detail::pinned_root::~pinned_root() = default;

std::string res::detail::write_beneath(pinned_root& root, const write_request& request)
{
	const auto parts = folder_parts(request.folder);
	if (!parts || !is_plain_name(request.leaf) || !is_plain_name(request.tmp_leaf)) {
		return std::string{ k_not_below };
	}
	fs::path reached;
	const std::vector<handle_holder> chain = hold_chain(root.path(), *parts, true, reached);
	if (chain.empty()) {
		return last_error_text("cannot open the folder (a reparse point, or not a folder)", reached);
	}
	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::folders_held, request.tmp_path);
	}
	const fs::path target = reached / request.leaf;
	// FILE_CREATE: nothing planted at the temp name is opened, not even a link.
	handle_holder file = open_in(chain.back().get(), request.tmp_leaf, FILE_GENERIC_WRITE | DELETE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE);
	if (!file.valid()) {
		return last_error_text("cannot create", request.tmp_path);
	}
	temp_guard guard{ file.get(), request.tmp_path };
	if (!lies_at(file.get(), chain.front().get(), *parts, request.tmp_leaf)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the temp file was not created below the resource root", request.tmp_path);
	}
	std::size_t done = 0;
	while (done < request.data.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(request.data.size() - done, 1u << 30));
		DWORD wrote = 0;
		if (!::WriteFile(file.get(), request.data.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
			return last_error_text("writing failed for", request.tmp_path);
		}
		done += wrote;
	}
	if (request.durable && !::FlushFileBuffers(file.get())) {
		return last_error_text("cannot flush", request.tmp_path);
	}

	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::before_rename, request.tmp_path);
	}
	// A file that was just removed (an erase on this thread) can stay delete-pending while another handle (watcher,
	// indexer, antivirus) is open on it, and replacing it is denied for a moment.
	bool renamed = false;
	for (int attempt = 0; attempt < 50 && !renamed; ++attempt) {
		renamed = rename_to(file.get(), chain.back().get(), request.leaf, request.replace);
		if (!renamed) {
			if (::GetLastError() != ERROR_ACCESS_DENIED) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	if (!renamed) {
		return last_error_text("cannot replace", target);
	}
	guard.dismiss();
	return {};
}

std::string res::detail::remove_beneath(pinned_root& root, const fs::path& folder, const fs::path& leaf, bool files_only)
{
	const auto parts = folder_parts(folder);
	if (!parts || !is_plain_name(leaf)) {
		return std::string{ k_not_below };
	}
	fs::path reached;
	const std::vector<handle_holder> chain = hold_chain(root.path(), *parts, false, reached);
	if (chain.empty()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
			? std::string{}
			: last_error_text("cannot open the folder (a reparse point, or not a folder)", reached);
	}
	const fs::path path = reached / leaf;
	handle_holder file = open_in(chain.back().get(), leaf, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
		files_only ? FILE_NON_DIRECTORY_FILE : 0);
	if (!file.valid()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? std::string{} : last_error_text("cannot remove", path);
	}
	if (!set_delete_on_close(file.get())) {
		return last_error_text("cannot remove", path);
	}
	return {};
}

std::string res::detail::rename_beneath(pinned_root& root, const rename_request& request)
{
	const auto from_parts = folder_parts(request.from_folder);
	const auto to_parts = folder_parts(request.to_folder);
	if (!from_parts || !to_parts || !is_plain_name(request.from_leaf) || !is_plain_name(request.to_leaf)) {
		return std::string{ k_not_below };
	}
	fs::path from_reached;
	const std::vector<handle_holder> from_chain = hold_chain(root.path(), *from_parts, false, from_reached);
	if (from_chain.empty()) {
		return last_error_text("cannot open the folder (a reparse point, or not a folder)", from_reached);
	}
	const fs::path source = from_reached / request.from_leaf;
	handle_holder file = open_in(from_chain.back().get(), request.from_leaf, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		FILE_OPEN, FILE_NON_DIRECTORY_FILE);
	if (!file.valid()) {
		return last_error_text("cannot find", source);
	}
	BY_HANDLE_FILE_INFORMATION information{};
	if (!::GetFileInformationByHandle(file.get(), &information)) {
		return last_error_text("cannot inspect", source);
	}
	if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("not a regular file", source);
	}
	if (!lies_at(file.get(), from_chain.front().get(), *from_parts, request.from_leaf)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the file was not found below the resource root", source);
	}
	fs::path to_reached;
	const std::vector<handle_holder> to_chain = hold_chain(root.path(), *to_parts, false, to_reached);
	if (to_chain.empty()) {
		return last_error_text("cannot open the target folder (missing, a reparse point, or not a folder)", to_reached);
	}
	if (!rename_to(file.get(), to_chain.back().get(), request.to_leaf, request.allow_replace)) {
		return last_error_text("cannot move to", to_reached / request.to_leaf);
	}
	return {};
}

#endif
