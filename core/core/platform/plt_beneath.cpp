#include <snk/platform/plt_beneath.h>
#include <snk/platform/plt_reparse_tag.h>
#include <snk/platform/plt_beneath_tree_hook.h>
#include <snk/platform/plt_link_check.h>
#include <snk/platform/plt_beneath_hold_hook.h>
#include <snk/platform/plt_windows_names.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/fs_paths.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
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
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <dirent.h>
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

	// The system call stops a name at a NUL: "..\0x" passes as a name here and opens "..".
	bool has_nul(const fs::path& path)
	{
		return path.native().find(fs::path::value_type{}) != fs::path::string_type::npos;
	}

	bool refused_by_windows([[maybe_unused]] const fs::path& name)
	{
#ifdef _WIN32
		return platform::beneath::windows_refuses_name(name.u16string());
#else
		return false;
#endif
	}

	// The folders below the root. safe_path never yields an absolute path or "..", but the walk does not rely on it.
	std::optional<std::vector<fs::path>> folder_parts(const fs::path& folder)
	{
		if (folder.has_root_path() || has_nul(folder)) {
			return std::nullopt;
		}
		std::vector<fs::path> parts;
		for (const auto& part : folder) {
			if (part == ".." || (part != "." && refused_by_windows(part))) {
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
		return !name.empty() && !has_nul(name) && !name.has_root_path() && name == name.filename() && name != "."
			&& name != ".." && !refused_by_windows(name);
	}

	constexpr std::string_view k_not_below = "the path is not a chain of folder names below the root";
	// remove_tree_beneath recurses once per folder: a tree nested past this is refused, not walked to a stack overflow.
	constexpr int k_tree_depth_limit = 256;

}

std::string platform::beneath::remove_tree_beneath(pinned_root& root, const fs::path& folder, const fs::path& leaf)
{
	return remove_tree_beneath_watched(root, folder, leaf, tree_hook{});
}

bool platform::beneath::may_be_short_name(const fs::path& name)
{
	const auto& text = name.native();
	for (std::size_t at = 0; at + 1 < text.size(); ++at) {
		if (text[at] == '~' && text[at + 1] >= '0' && text[at + 1] <= '9') {
			return true;
		}
	}
	return false;
}

namespace {

	std::mutex g_hold_hook_mutex;
	platform::beneath::hold_hook g_hold_hook;
	std::atomic<bool> g_proc_fd_unusable{ false };

	void call_hold_hook(const fs::path& folder)
	{
		platform::beneath::hold_hook hook;
		{
			const std::lock_guard lock(g_hold_hook_mutex);
			hook = g_hold_hook;
		}
		if (hook) {
			hook(folder);
		}
	}

	platform::beneath::hold_result hold_refused(platform::beneath::hold_status status, std::string why)
	{
		platform::beneath::hold_result result;
		result.status = status;
		result.error = std::move(why);
		return result;
	}

	// Where `name` is among the held names; nullopt when it is not one of them.
	std::optional<std::size_t> held_index(const std::vector<fs::path>& names, const fs::path& name)
	{
		const auto found = std::ranges::find(names, name);
		if (found == names.end()) {
			return std::nullopt;
		}
		return static_cast<std::size_t>(std::distance(names.begin(), found));
	}

}

void platform::beneath::set_hold_hook(hold_hook hook)
{
	const std::lock_guard lock(g_hold_hook_mutex);
	g_hold_hook = std::move(hook);
}

void platform::beneath::set_proc_fd_unusable(bool unusable)
{
	g_proc_fd_unusable = unusable;
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
		// Not atomic: an entry made at the target between the look-up and renameat is replaced. Callers that rely on
		// "never replaces" to take a name exclusively (a migration journal) are exclusive only where the flag works.
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

platform::beneath::pinned_root::~pinned_root()
{
	if (m_fd >= 0) {
		::close(m_fd);
	}
}

int platform::beneath::pinned_root::descriptor(std::string& error)
{
	return open_root(error, true);
}

int platform::beneath::pinned_root::existing_descriptor(std::string& error)
{
	return open_root(error, false);
}

int platform::beneath::pinned_root::open_root(std::string& error, bool create)
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
		if (create) {
			std::error_code ec;
			fs::create_directories(m_root, ec);
		}
		m_fd = ::open(m_root.c_str(), k_dir_flags);
		if (m_fd < 0) {
			const int code = errno;
			error = errno_text("cannot open the root", m_root);
			errno = code;
		}
	}
	return m_fd;
}

std::string platform::beneath::write_beneath(pinned_root& root, const write_request& request)
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
	if (request.stream) {
		const chunk_sink sink = [&file](std::span<const std::byte> piece) { return write_all(file.get(), piece); };
		if (!(*request.stream)(sink)) {
			return errno_text("writing failed for", request.tmp_path);
		}
	} else if (!write_all(file.get(), request.data)) {
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

std::string platform::beneath::remove_beneath(pinned_root& root, const fs::path& folder, const fs::path& leaf, bool files_only)
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
	if (files_only) {
		// Again on the io thread: the check before the post is long past, and unlinkat removes a link, a FIFO or a socket.
		struct stat entry;
		if (::fstatat(at.get(), leaf.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
			return errno == ENOENT ? std::string{} : errno_text("cannot inspect", reached / leaf);
		}
		if (!S_ISREG(entry.st_mode)) {
			return std::format("'{}' is not a regular file", fs_paths::utf8_of(reached / leaf));
		}
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

namespace {

	// The entries of the folder `fd` holds, "." and ".." left out, read through a descriptor of its own. False with
	// errno set when it cannot be read.
	bool list_folder(int fd, std::vector<std::string>& names)
	{
		const int copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
		if (copy < 0) {
			return false;
		}
		DIR* folder = ::fdopendir(copy);
		if (!folder) {
			const int error = errno;
			::close(copy);
			errno = error;
			return false;
		}
		errno = 0;
		while (const dirent* entry = ::readdir(folder)) {
			const std::string_view name = entry->d_name;
			if (name != "." && name != "..") {
				names.emplace_back(name);
			}
			errno = 0;
		}
		const int error = errno;
		::closedir(folder);
		errno = error;
		return error == 0;
	}

	// Empties the folder `name` in `parent` and removes it. The folder is opened with O_NOFOLLOW (a link swapped in at
	// its name is refused), its entries are listed before any goes and each is looked at without following a link: a
	// folder is entered the same way, anything else (a link among them) is unlinked as itself. A folder on another
	// device than `device` (a mount point) is refused, not emptied.
	std::string remove_tree_at(int parent, const std::string& name, const fs::path& path,
		const platform::beneath::tree_hook* hook, int depth, dev_t device)
	{
		if (depth > k_tree_depth_limit) {
			return std::format("'{}' is {} {} folders", fs_paths::utf8_of(path), platform::beneath::k_tree_too_deep,
				k_tree_depth_limit);
		}
		const fd_holder folder{ ::openat(parent, name.c_str(), k_dir_flags) };
		if (folder.get() < 0) {
			// Gone since its parent was listed: as removed as it will be.
			return errno == ENOENT ? std::string{} : errno_text("cannot open the folder (a link, or not a folder)",
				path);
		}
		struct stat held;
		if (::fstat(folder.get(), &held) != 0) {
			return errno_text("cannot inspect", path);
		}
		if (held.st_dev != device) {
			return std::format("'{}' is on another device (a mount point): it is not removed", fs_paths::utf8_of(path));
		}
		if (hook && *hook) {
			(*hook)(path);
		}
		std::vector<std::string> names;
		if (!list_folder(folder.get(), names)) {
			return errno_text("cannot list", path);
		}
		for (const auto& entry : names) {
			struct stat information;
			if (::fstatat(folder.get(), entry.c_str(), &information, AT_SYMLINK_NOFOLLOW) != 0) {
				if (errno == ENOENT) {
					continue;
				}
				return errno_text("cannot inspect", path / entry);
			}
			if (S_ISDIR(information.st_mode)) {
				std::string error = remove_tree_at(folder.get(), entry, path / entry, hook, depth + 1, device);
				if (!error.empty()) {
					return error;
				}
			} else if (::unlinkat(folder.get(), entry.c_str(), 0) != 0 && errno != ENOENT) {
				return errno_text("cannot remove", path / entry);
			}
		}
		// A link swapped in at the name since it was opened is not a folder: AT_REMOVEDIR refuses it.
		if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT) {
			return errno_text("cannot remove", path);
		}
		return {};
	}

}

std::string platform::beneath::make_folders_beneath(pinned_root& root, const fs::path& folder, bool must_be_new)
{
	if (must_be_new) {
		// The umask applies, as it did to the folders std::filesystem made.
		return make_new_folder_beneath(root, folder, 0777);
	}
	const auto parts = folder_parts(folder);
	if (!parts) {
		return std::string{ k_not_below };
	}
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path reached = root.path();
	const fd_holder made = open_below(root_fd, *parts, true, reached);
	return made.get() < 0 ? errno_text("cannot make the folder (a link, or not a folder, on the way)", reached)
						  : std::string{};
}

std::string platform::beneath::make_new_folder_beneath(pinned_root& root, const fs::path& folder, std::uint32_t mode,
	folder_identity* made)
{
	auto parts = folder_parts(folder);
	if (!parts || parts->empty()) {
		return std::string{ k_not_below };
	}
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return error;
	}
	fs::path reached = root.path();
	const fs::path last = parts->back();
	parts->pop_back();
	const fd_holder parent = open_below(root_fd, *parts, true, reached);
	if (parent.get() < 0) {
		return errno_text("cannot make the folder (a link, or not a folder, on the way)", reached);
	}
	reached /= last;
	if (::mkdirat(parent.get(), last.c_str(), static_cast<mode_t>(mode)) != 0) {
		return errno == EEXIST ? std::format("'{}' already exists", fs_paths::utf8_of(reached))
							   : errno_text("cannot make", reached);
	}
	if (made) {
		const fd_holder opened{ ::openat(parent.get(), last.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) };
		struct stat information;
		if (opened.get() < 0 || ::fstat(opened.get(), &information) != 0) {
			return errno_text("cannot inspect the folder just made", reached);
		}
		*made = { static_cast<std::uint64_t>(information.st_ino), 0, static_cast<std::uint64_t>(information.st_dev) };
	}
	return {};
}

std::string platform::beneath::remove_tree_beneath_watched(pinned_root& root, const fs::path& folder,
	const fs::path& leaf, const tree_hook& watch)
{
	const tree_hook* const hook = &watch;
	const auto parts = folder_parts(folder);
	if (!parts || !is_plain_name(leaf)) {
		return std::string{ k_not_below };
	}
	std::string error;
	const int root_fd = root.existing_descriptor(error);
	if (root_fd < 0) {
		return errno == ENOENT ? std::string{} : error;
	}
	fs::path reached = root.path();
	const fd_holder at = open_below(root_fd, *parts, false, reached);
	if (at.get() < 0) {
		return errno == ENOENT ? std::string{} : errno_text("cannot open the folder (a link, or not a folder)", reached);
	}
	const fs::path path = reached / leaf;
	struct stat information;
	if (::fstatat(at.get(), leaf.c_str(), &information, AT_SYMLINK_NOFOLLOW) != 0) {
		return errno == ENOENT ? std::string{} : errno_text("cannot inspect", path);
	}
	if (S_ISLNK(information.st_mode)) {
		return std::format("'{}' is a link: only a real folder is removed as a tree", fs_paths::utf8_of(path));
	}
	if (!S_ISDIR(information.st_mode)) {
		const bool removed = ::unlinkat(at.get(), leaf.c_str(), 0) == 0 || errno == ENOENT;
		return removed ? std::string{} : errno_text("cannot remove", path);
	}
	struct stat parent;
	if (::fstat(at.get(), &parent) != 0) {
		return errno_text("cannot inspect", reached);
	}
	return remove_tree_at(at.get(), leaf.string(), path, hook, 0, parent.st_dev);
}

namespace {

	platform::beneath::entry_kind kind_of_mode(mode_t mode)
	{
		using platform::beneath::entry_kind;
		if (S_ISLNK(mode)) {
			return entry_kind::link;
		}
		if (S_ISDIR(mode)) {
			return entry_kind::folder;
		}
		return S_ISREG(mode) ? entry_kind::file : entry_kind::other;
	}

	platform::beneath::entry_kind kind_of_type(unsigned char type)
	{
		using platform::beneath::entry_kind;
		switch (type) {
		case DT_REG:
			return entry_kind::file;
		case DT_DIR:
			return entry_kind::folder;
		case DT_LNK:
			return entry_kind::link;
		default:
			return entry_kind::other;
		}
	}

}

platform::beneath::list_result platform::beneath::list_beneath(pinned_root& root, const fs::path& folder, std::size_t max_entries)
{
	list_result out;
	const auto parts = folder_parts(folder);
	if (!parts) {
		out.error = std::string{ k_not_below };
		return out;
	}
	std::string error;
	const int root_fd = root.existing_descriptor(error);
	if (root_fd < 0) {
		if (errno == ENOENT) {
			out.missing = true;
		} else {
			out.error = std::move(error);
		}
		return out;
	}
	fs::path reached = root.path();
	const fd_holder at = open_below(root_fd, *parts, false, reached);
	if (at.get() < 0) {
		if (errno == ENOENT) {
			out.missing = true;
		} else {
			out.error = errno_text("cannot open the folder (a link, or not a folder)", reached);
		}
		return out;
	}
	const int copy = ::fcntl(at.get(), F_DUPFD_CLOEXEC, 0);
	if (copy < 0) {
		out.error = errno_text("cannot list", reached);
		return out;
	}
	DIR* listing = ::fdopendir(copy);
	if (!listing) {
		const int code = errno;
		::close(copy);
		errno = code;
		out.error = errno_text("cannot list", reached);
		return out;
	}
	int code = 0;
	for (;;) {
		errno = 0;
		const dirent* entry = ::readdir(listing);
		if (!entry) {
			code = errno;
			break;
		}
		const std::string_view name = entry->d_name;
		if (name == "." || name == "..") {
			continue;
		}
		if (out.entries.size() >= max_entries) {
			out.truncated = true;
			break;
		}
		listed_entry item;
		item.name = fs::path(std::string(name));
		if (entry->d_type == DT_UNKNOWN) {
			struct stat information;
			if (::fstatat(at.get(), entry->d_name, &information, AT_SYMLINK_NOFOLLOW) == 0) {
				item.kind = kind_of_mode(information.st_mode);
			}
		} else {
			item.kind = kind_of_type(entry->d_type);
		}
		out.entries.push_back(std::move(item));
	}
	::closedir(listing);
	if (code != 0) {
		errno = code;
		out.error = errno_text("cannot list", reached);
		out.entries.clear();
		out.truncated = false;
	}
	return out;
}

std::string platform::beneath::rename_beneath(pinned_root& root, const rename_request& request)
{
	if (request.across_volumes) {
		*request.across_volumes = false;
	}
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
		const int code = errno;
		if (request.across_volumes) {
			*request.across_volumes = code == EXDEV;
		}
		errno = code;
		return errno_text("cannot move to", to_reached / request.to_leaf);
	}
	return {};
}

namespace {
	std::int64_t nanoseconds_of(const struct timespec& time)
	{
		return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + static_cast<std::int64_t>(time.tv_nsec);
	}

	platform::beneath::file_stamp stamp_of(const struct stat& information)
	{
#if defined(__APPLE__)
		const struct timespec& written = information.st_mtimespec;
		const struct timespec& changed = information.st_ctimespec;
#else
		const struct timespec& written = information.st_mtim;
		const struct timespec& changed = information.st_ctim;
#endif
		return { static_cast<std::uint64_t>(information.st_size), nanoseconds_of(written), nanoseconds_of(changed),
			static_cast<std::uint64_t>(information.st_ino), static_cast<std::uint64_t>(information.st_dev) };
	}

	// The stamp taken at the open, if the same descriptor still shows it and `read_bytes` were read: else the file
	// changed while it was read.
	std::optional<platform::beneath::file_stamp> stamp_after_read(int file,
		const std::optional<platform::beneath::file_stamp>& opened, std::uint64_t read_bytes)
	{
		struct stat information;
		if (!opened || ::fstat(file, &information) != 0 || stamp_of(information) != *opened || read_bytes != opened->size) {
			return std::nullopt;
		}
		return opened;
	}

	// What read_beneath and read_beneath_stream share: the file of `request` opened as read_beneath describes; `file_out`,
	// `size_out`, `stamp_out` and `path_out` are set when the result is ok.
	platform::beneath::read_result open_for_read(const platform::beneath::read_request& request, fd_holder& file_out,
		std::uint64_t& size_out, std::optional<platform::beneath::file_stamp>& stamp_out, fs::path& path_out)
	{
		using namespace platform::beneath;
		const auto parts = folder_parts(request.folder);
		if (!parts || !is_plain_name(request.leaf)) {
			return { read_status::failed, std::string{ k_not_below } };
		}
		if (request.hook && *request.hook) {
			(*request.hook)(read_stage::before_walk);
		}
		const fd_holder root{ ::open(request.root.c_str(), k_dir_flags) };
		if (root.get() < 0) {
			return { errno == ENOENT ? read_status::missing : read_status::failed, errno_text("cannot open the root", request.root) };
		}
		fs::path reached = request.root;
		const fd_holder folder = open_below(root.get(), *parts, false, reached);
		if (folder.get() < 0) {
			return { errno == ENOENT ? read_status::missing : read_status::failed,
				errno_text("cannot open the folder (a link, or not a folder)", reached) };
		}
		if (request.hook && *request.hook) {
			(*request.hook)(read_stage::folders_held);
		}
		// O_NONBLOCK: opening a FIFO without a writer, or a device, must not wait; the type is checked right after.
		file_out = fd_holder{ ::openat(folder.get(), request.leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC) };
		if (file_out.get() < 0) {
			if (errno == ENOENT) {
				return { read_status::missing, {} };
			}
			// ELOOP: the name is a link.
			return { errno == ELOOP ? read_status::not_regular : read_status::failed, errno_text("cannot open", reached / request.leaf) };
		}
		struct stat information;
		if (::fstat(file_out.get(), &information) != 0) {
			return { read_status::failed, errno_text("cannot inspect", reached / request.leaf) };
		}
		if (!S_ISREG(information.st_mode)) {
			return { read_status::not_regular, {} };
		}
		// A regular file now: back to blocking reads, which FUSE and network file systems may need.
		const int flags = ::fcntl(file_out.get(), F_GETFL);
		if (flags < 0 || ::fcntl(file_out.get(), F_SETFL, flags & ~O_NONBLOCK) != 0) {
			return { read_status::failed, errno_text("cannot inspect", reached / request.leaf) };
		}
		if (information.st_size < 0 || static_cast<std::uint64_t>(information.st_size) > request.max_bytes) {
			return { read_status::too_large, {} };
		}
		size_out = static_cast<std::uint64_t>(information.st_size);
		stamp_out = stamp_of(information);
		path_out = reached / request.leaf;
		return { read_status::ok, {} };
	}
}

platform::beneath::read_result platform::beneath::read_beneath(const read_request& request, std::vector<std::byte>& out)
{
	out.clear();
	fd_holder file;
	std::uint64_t size = 0;
	std::optional<file_stamp> stamp;
	fs::path path;
	if (read_result opened = open_for_read(request, file, size, stamp, path); opened.status != read_status::ok) {
		return opened;
	}
	if (request.stamp_only) {
		read_result result{ read_status::ok, {} };
		result.stamp = stamp;
		return result;
	}
	out.resize(static_cast<std::size_t>(size));
	std::size_t done = 0;
	while (done < out.size()) {
		const ssize_t got = ::read(file.get(), out.data() + done, out.size() - done);
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			out.clear();
			return { read_status::failed, errno_text("cannot read", path) };
		}
		if (got == 0) {
			break; // shortened since fstat: what is there now
		}
		done += static_cast<std::size_t>(got);
	}
	out.resize(done);
	read_result result{ read_status::ok, {} };
	result.stamp = stamp_after_read(file.get(), stamp, done);
	return result;
}

platform::beneath::read_result platform::beneath::read_beneath_stream(const read_request& request,
	const std::function<bool(std::span<const std::byte>)>& sink)
{
	fd_holder file;
	std::uint64_t size = 0;
	std::optional<file_stamp> stamp;
	fs::path path;
	if (read_result opened = open_for_read(request, file, size, stamp, path); opened.status != read_status::ok) {
		return opened;
	}
	std::vector<std::byte> buffer(64 * 1024);
	std::uint64_t total = 0;
	for (;;) {
		const ssize_t got = ::read(file.get(), buffer.data(), buffer.size());
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			return { read_status::failed, errno_text("cannot read", path) };
		}
		if (got == 0) {
			break;
		}
		total += static_cast<std::uint64_t>(got);
		if (total > request.max_bytes) {
			return { read_status::too_large, {} };
		}
		if (!sink(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(got)))) {
			break;
		}
	}
	read_result result{ read_status::ok, {} };
	result.stamp = stamp_after_read(file.get(), stamp, total);
	return result;
}

fs::path platform::beneath::long_path(const fs::path& path)
{
	return path;
}

std::optional<platform::beneath::link_entry> platform::beneath::link_entry_at(const fs::path& path, std::error_code& ec)
{
	ec.clear();
	struct stat information;
	if (::lstat(path.c_str(), &information) != 0) {
		if (errno != ENOENT && errno != ENOTDIR) {
			ec = std::error_code(errno, std::generic_category());
		}
		return std::nullopt;
	}
	return S_ISLNK(information.st_mode) ? link_entry::link : link_entry::plain;
}

struct platform::beneath::held_folder::state {
	std::vector<fs::path> names;
	fd_holder folder;
	std::vector<fd_holder> files;
	// "/proc/self/fd/<folder>" when it leads to the held files; empty otherwise.
	std::string proc_folder;
};

platform::beneath::held_folder::held_folder(std::unique_ptr<state> held)
	: m_state(std::move(held))
{
}

platform::beneath::held_folder::~held_folder() = default;

const std::vector<fs::path>& platform::beneath::held_folder::names() const
{
	return m_state->names;
}

fs::path platform::beneath::held_folder::load_path(const fs::path& name) const
{
	if (m_state->proc_folder.empty() || !held_index(m_state->names, name)) {
		return {};
	}
	return fs::path{ m_state->proc_folder } / name;
}

bool platform::beneath::held_folder::read(const fs::path& name, const chunk_sink& sink) const
{
	const auto index = held_index(m_state->names, name);
	if (!index) {
		return false;
	}
	const int file = m_state->files[*index].get();
	std::vector<std::byte> buffer(64 * 1024);
	off_t offset = 0;
	for (;;) {
		const ssize_t got = ::pread(file, buffer.data(), buffer.size(), offset);
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (got == 0) {
			return true;
		}
		offset += static_cast<off_t>(got);
		if (!sink(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(got)))) {
			return false;
		}
	}
}

platform::beneath::hold_result platform::beneath::hold_folder_beneath(pinned_root& root, const fs::path& folder,
	const folder_identity& made, std::span<const fs::path> names)
{
	const auto parts = folder_parts(folder);
	if (!parts || parts->empty() || !std::ranges::all_of(names, is_plain_name)) {
		return hold_refused(hold_status::failed, std::string{ k_not_below });
	}
	std::string error;
	const int root_fd = root.descriptor(error);
	if (root_fd < 0) {
		return hold_refused(hold_status::failed, error);
	}
	fs::path reached = root.path();
	auto kept = std::make_unique<held_folder::state>();
	kept->names.assign(names.begin(), names.end());
	kept->folder = open_below(root_fd, *parts, false, reached);
	if (kept->folder.get() < 0) {
		return hold_refused(hold_status::failed,
			errno_text("cannot open the folder (a link, or not a folder)", reached));
	}
	const uid_t self = ::geteuid();
	struct stat information;
	if (::fstat(kept->folder.get(), &information) != 0) {
		return hold_refused(hold_status::failed, errno_text("cannot inspect", reached));
	}
	const folder_identity held_identity{ static_cast<std::uint64_t>(information.st_ino), 0,
		static_cast<std::uint64_t>(information.st_dev) };
	if (held_identity != made) {
		return hold_refused(hold_status::failed, std::format("'{}' is not the folder made for the copy",
			fs_paths::utf8_of(reached)));
	}
	if (information.st_uid != self || (information.st_mode & 077) != 0) {
		return hold_refused(hold_status::not_private, std::format("'{}' is not a folder of this user closed to others "
			"(owner {}, mode {:o})", fs_paths::utf8_of(reached), information.st_uid, information.st_mode & 07777));
	}
	std::vector<std::string> present;
	if (!list_folder(kept->folder.get(), present)) {
		return hold_refused(hold_status::failed, errno_text("cannot list", reached));
	}
	for (const auto& entry : present) {
		if (std::ranges::find(names, fs::path{ entry }) == names.end()) {
			return hold_refused(hold_status::failed, std::format("'{}' holds '{}', which was not put there",
				fs_paths::utf8_of(reached), entry));
		}
	}
	std::vector<struct stat> seen;
	seen.reserve(names.size());
	for (const auto& name : names) {
		const fs::path path = reached / name;
		fd_holder file{ ::openat(kept->folder.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) };
		if (file.get() < 0) {
			return hold_refused(hold_status::failed, errno_text("cannot open (a link, or missing)", path));
		}
		if (::fstat(file.get(), &information) != 0) {
			return hold_refused(hold_status::failed, errno_text("cannot inspect", path));
		}
		if (!S_ISREG(information.st_mode)) {
			return hold_refused(hold_status::failed, std::format("'{}' is not a regular file", fs_paths::utf8_of(path)));
		}
		if (information.st_uid != self) {
			return hold_refused(hold_status::not_private, std::format("'{}' is not a file of this user (owner {})",
				fs_paths::utf8_of(path), information.st_uid));
		}
		if (::fchmod(file.get(), S_IRUSR) != 0 || ::fstat(file.get(), &information) != 0
			|| (information.st_mode & 07777) != S_IRUSR) {
			return hold_refused(hold_status::not_private, std::format("'{}' cannot be made readable by this user "
				"alone (mode {:o})", fs_paths::utf8_of(path), information.st_mode & 07777));
		}
		seen.push_back(information);
		kept->files.push_back(std::move(file));
	}
#if defined(__linux__)
	if (!g_proc_fd_unusable) {
		// The loader is given this path: it must reach the very files held, not what another /proc would show.
		std::string proc = std::format("/proc/self/fd/{}", kept->folder.get());
		bool usable = true;
		for (std::size_t index = 0; index < names.size() && usable; ++index) {
			struct stat via;
			usable = ::stat((fs::path{ proc } / names[index]).c_str(), &via) == 0 && via.st_dev == seen[index].st_dev
				&& via.st_ino == seen[index].st_ino;
		}
		if (usable) {
			kept->proc_folder = std::move(proc);
		}
	}
#endif
	hold_result result;
	result.status = hold_status::held;
	result.held = std::make_shared<held_folder>(std::move(kept));
	call_hold_hook(folder);
	return result;
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
	// `security`: the descriptor an entry this call makes gets (null: inherited from the folder above).
	handle_holder open_in(HANDLE parent, const fs::path& name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
		bool open_reparse_point = true, PSECURITY_DESCRIPTOR security = nullptr)
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
		attributes.SecurityDescriptor = security;
		IO_STATUS_BLOCK status_block{};
		HANDLE handle = nullptr;
		// FILE_READ_ATTRIBUTES as CreateFileW adds it: GetFileInformationByHandle and GetFinalPathNameByHandleW need it.
		const NTSTATUS status = ::NtCreateFile(&handle, access | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &attributes, &status_block, nullptr,
			FILE_ATTRIBUTE_NORMAL, share, disposition,
			options | FILE_SYNCHRONOUS_IO_NONALERT | (open_reparse_point ? FILE_OPEN_REPARSE_POINT : 0), nullptr, 0);
		if (status < 0) {
			::SetLastError(::RtlNtStatusToDosError(status));
			return handle_holder{};
		}
		return handle_holder{ handle };
	}

	// The TOKEN_USER of this process; empty when it cannot be read.
	std::vector<std::byte> current_token_user()
	{
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return {};
		}
		const handle_holder held{ token };
		DWORD size = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<std::byte> buffer(size);
		if (size == 0 || !::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
			return {};
		}
		return buffer;
	}

	PSID user_sid(const std::vector<std::byte>& token_user)
	{
		return reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid;
	}

	// A security descriptor the system allocated, freed with LocalFree.
	struct local_descriptor
	{
		PSECURITY_DESCRIPTOR descriptor = nullptr;

		local_descriptor() = default;
		explicit local_descriptor(PSECURITY_DESCRIPTOR owned) : descriptor(owned) {}
		~local_descriptor()
		{
			if (descriptor) {
				::LocalFree(descriptor);
			}
		}
		local_descriptor(const local_descriptor&) = delete;
		local_descriptor& operator=(const local_descriptor&) = delete;
	};

	// The Windows side of a folder made 0700 (#1304): owned by the current user, a protected DACL (nothing inherited
	// from the project's folders) with one entry granting that user everything, inherited by what is made inside.
	// False with the last error set when it cannot be built.
	bool private_descriptor(local_descriptor& out)
	{
		const std::vector<std::byte> token_user = current_token_user();
		if (token_user.empty()) {
			return false;
		}
		LPWSTR text = nullptr;
		if (!::ConvertSidToStringSidW(user_sid(token_user), &text)) {
			return false;
		}
		const std::wstring sid{ text };
		::LocalFree(text);
		const std::wstring sddl = L"O:" + sid + L"D:P(A;OICI;FA;;;" + sid + L")";
		return ::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &out.descriptor,
			nullptr) != 0;
	}

	// Why the folder `folder` (opened with READ_CONTROL) is not closed to everyone but the current user: not owned by
	// them, a DACL that is not protected (inherits from above), or an allowing entry for anyone else. Empty when it is.
	std::string not_private_reason(HANDLE folder)
	{
		const std::vector<std::byte> token_user = current_token_user();
		if (token_user.empty()) {
			return "the current user cannot be read";
		}
		const PSID self = user_sid(token_user);
		PSID owner = nullptr;
		PACL dacl = nullptr;
		PSECURITY_DESCRIPTOR raw = nullptr;
		const DWORD status = ::GetSecurityInfo(folder, SE_FILE_OBJECT,
			OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &raw);
		if (status != ERROR_SUCCESS) {
			return std::format("its permissions cannot be read: {}",
				std::system_category().message(static_cast<int>(status)));
		}
		const local_descriptor descriptor{ raw };
		if (!owner || !::EqualSid(owner, self)) {
			return "it is not owned by this user";
		}
		SECURITY_DESCRIPTOR_CONTROL control = 0;
		DWORD revision = 0;
		if (!::GetSecurityDescriptorControl(descriptor.descriptor, &control, &revision)
			|| (control & SE_DACL_PROTECTED) == 0) {
			return "its permissions are inherited from the folder above";
		}
		if (!dacl) {
			return "it has no access list: everyone may use it";
		}
		for (DWORD index = 0; index < dacl->AceCount; ++index) {
			void* ace = nullptr;
			if (!::GetAce(dacl, index, &ace)) {
				return "its access list cannot be read";
			}
			const auto* header = static_cast<const ACE_HEADER*>(ace);
			if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
				continue;
			}
			if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
				return "its access list holds an entry of another kind";
			}
			auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
			if (!::EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), self)) {
				return "its access list lets another account in";
			}
		}
		return {};
	}

	bool same_file(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b)
	{
		return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh
			&& a.nFileIndexLow == b.nFileIndexLow;
	}

	// FileIdInfo and FILE_ID_INFO (Windows 8), spelled out: the SDK declares them only for a high enough _WIN32_WINNT.
	constexpr auto k_file_id_info = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
	struct file_id_info {
		ULONGLONG volume_serial_number;
		BYTE identifier[16];
	};

	// False with the last error set when neither the file ID nor the file index can be read.
	bool identity_of(HANDLE file, platform::beneath::folder_identity& out)
	{
		file_id_info id{};
		if (::GetFileInformationByHandleEx(file, k_file_id_info, &id, sizeof(id))) {
			std::uint64_t halves[2]{};
			static_assert(sizeof(halves) == sizeof(id.identifier));
			std::memcpy(halves, id.identifier, sizeof(halves));
			out = { halves[0], halves[1], static_cast<std::uint64_t>(id.volume_serial_number) };
			return true;
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(file, &information)) {
			return false;
		}
		out = { (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow, 0,
			static_cast<std::uint64_t>(information.dwVolumeSerialNumber) };
		return true;
	}

	enum class reparse_kind { none, redirect, filter, unknown };

	// What `handle` (opened as the reparse point itself) is: a name surrogate (link, junction) redirects, any other
	// reparse point is a filter's file kept in place (OneDrive/Cloud Files placeholder, dedup, WCI).
	reparse_kind reparse_of(HANDLE handle, const BY_HANDLE_FILE_INFORMATION& information)
	{
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
			return reparse_kind::none;
		}
		FILE_ATTRIBUTE_TAG_INFO tag{};
		if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
			return reparse_kind::unknown;
		}
		return platform::beneath::reparse_tag_redirects(tag.ReparseTag) ? reparse_kind::redirect : reparse_kind::filter;
	}

	// A folder of the walk, reads (#484) and writes (#523) alike: `held` was opened as the reparse point itself. A folder
	// a filter keeps in place (a OneDrive placeholder) is opened again through the filter by `reopen` and kept only if
	// that is the same folder; a link or a junction is refused (platform::beneath::folder_entry_of). Invalid with the
	// last error set when refused.
	handle_holder enter_folder(handle_holder held, const std::function<handle_holder()>& reopen)
	{
		BY_HANDLE_FILE_INFORMATION information{};
		if (!held.valid() || !::GetFileInformationByHandle(held.get(), &information)) {
			return handle_holder{};
		}
		FILE_ATTRIBUTE_TAG_INFO tag{};
		const bool tag_known = (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
			|| ::GetFileInformationByHandleEx(held.get(), FileAttributeTagInfo, &tag, sizeof(tag)) != 0;
		switch (platform::beneath::folder_entry_of(information.dwFileAttributes, tag_known, tag.ReparseTag)) {
		case platform::beneath::folder_entry::enter:
			return held;
		case platform::beneath::folder_entry::through_filter:
			break;
		case platform::beneath::folder_entry::not_a_folder:
			::SetLastError(ERROR_DIRECTORY);
			return handle_holder{};
		case platform::beneath::folder_entry::refused:
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return handle_holder{};
		}
		handle_holder followed = reopen();
		BY_HANDLE_FILE_INFORMATION after{};
		if (!followed.valid() || !::GetFileInformationByHandle(followed.get(), &after)) {
			return handle_holder{};
		}
		if (!same_file(information, after) || (after.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return handle_holder{};
		}
		return followed;
	}

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

	// When Windows gives no final path (some network redirectors): a write or a delete is refused, a read goes on if no
	// name could be a short (8.3) alias.
	enum class unnormalized { refuse, check_names };

	// Belt and braces over the handle-relative opens: the final path of `file` is `root_path`, the folders and the name,
	// so no short (8.3) name or other alias reached it.
	bool lies_at(HANDLE file, const std::optional<std::wstring>& root_path, std::span<const fs::path> parts, const fs::path& name,
		unnormalized fallback)
	{
		const auto actual = final_path(file);
		if (!actual || !root_path) {
			const auto plain = [](const fs::path& part) { return !platform::beneath::may_be_short_name(part); };
			return fallback == unnormalized::check_names && std::ranges::all_of(parts, plain) && plain(name);
		}
		std::wstring expected = *root_path;
		if (expected.ends_with(L'\\')) {
			expected.pop_back(); // the root of a drive ends with its separator
		}
		for (const auto& part : parts) {
			expected.append(L"\\").append(part.native());
		}
		expected.append(L"\\").append(name.native());
		return ::CompareStringOrdinal(actual->c_str(), static_cast<int>(actual->size()), expected.c_str(),
			static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
	}

	// Held without FILE_SHARE_DELETE: while the handle is open nobody can rename, remove or replace the folder.
	constexpr ACCESS_MASK k_folder_access = FILE_LIST_DIRECTORY | FILE_TRAVERSE;
	constexpr ULONG k_folder_share = FILE_SHARE_READ | FILE_SHARE_WRITE;

	struct held_chain {
		std::vector<handle_holder> folders;
		// The root's final path, nullopt when Windows gives none.
		std::optional<std::wstring> root_path;

		bool empty() const { return folders.empty(); }
		HANDLE last() const { return folders.back().get(); }
	};

	// The root and every folder down to `parts`, each opened from the handle of the one above it and all held until
	// the chain goes; empty on failure, `reached` naming where it stopped.
	// A folder a file system filter keeps as a reparse point (a cloud placeholder) is entered through the filter
	// (enter_folder), for writes too since #523; a link or a junction is refused anywhere.
	// Each folder must lie at its names (lies_at) before the next is opened or made: a short (8.3) alias (GIT~1) would
	// otherwise lead into another folder, and with `create` make folders there.
	held_chain hold_chain(const fs::path& root, const std::vector<fs::path>& parts, bool create, fs::path& reached, unnormalized fallback)
	{
		held_chain chain;
		// Closing what was held must not change the error the caller reports.
		const auto give_up = [&chain] {
			const DWORD code = ::GetLastError();
			chain.folders.clear();
			::SetLastError(code);
			return held_chain{};
		};
		if (create) {
			std::error_code ec;
			fs::create_directories(root, ec); // the mounted folder itself, pinned when mounted
		}
		reached = root;
		handle_holder top{ ::CreateFileW(root.c_str(), k_folder_access | SYNCHRONIZE, k_folder_share, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
		top = enter_folder(std::move(top), [&root] {
			return handle_holder{ ::CreateFileW(root.c_str(), k_folder_access | SYNCHRONIZE, k_folder_share, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
		});
		if (!top.valid()) {
			return give_up();
		}
		chain.root_path = final_path(top.get());
		chain.folders.push_back(std::move(top));
		const std::span<const fs::path> all_parts(parts);
		for (std::size_t index = 0; index < parts.size(); ++index) {
			const fs::path& part = parts[index];
			reached /= part;
			const HANDLE parent = chain.last();
			handle_holder next = open_in(parent, part, k_folder_access, k_folder_share, create ? FILE_OPEN_IF : FILE_OPEN, FILE_DIRECTORY_FILE);
			next = enter_folder(std::move(next), [parent, &part] {
				return open_in(parent, part, k_folder_access, k_folder_share, FILE_OPEN, FILE_DIRECTORY_FILE, false);
			});
			if (!next.valid()) {
				return give_up();
			}
			if (!lies_at(next.get(), chain.root_path, all_parts.first(index), part, fallback)) {
				::SetLastError(ERROR_CANT_ACCESS_FILE);
				return give_up();
			}
			chain.folders.push_back(std::move(next));
		}
		return chain;
	}

	// Before a rename that may replace `leaf`: what is there now lies at its name, so a short (8.3) alias does not
	// replace another file. Nothing there lets the rename go on; so does a file being deleted (opening one is denied),
	// unless the name could be an alias, which then cannot be checked.
	bool replaced_leaf_lies_at(const held_chain& chain, std::span<const fs::path> parts, const fs::path& leaf)
	{
		const handle_holder existing = open_in(chain.last(), leaf, FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, 0);
		if (!existing.valid()) {
			const DWORD code = ::GetLastError();
			if (code == ERROR_ACCESS_DENIED) {
				return !platform::beneath::may_be_short_name(leaf);
			}
			return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
		}
		return lies_at(existing.get(), chain.root_path, parts, leaf, unnormalized::refuse);
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

platform::beneath::pinned_root::~pinned_root() = default;

std::string platform::beneath::write_beneath(pinned_root& root, const write_request& request)
{
	const auto parts = folder_parts(request.folder);
	if (!parts || !is_plain_name(request.leaf) || !is_plain_name(request.tmp_leaf)) {
		return std::string{ k_not_below };
	}
	fs::path reached;
	const held_chain chain = hold_chain(root.path(), *parts, true, reached, unnormalized::refuse);
	if (chain.empty()) {
		return last_error_text("cannot open the folder (a link, a junction, or not a folder)", reached);
	}
	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::folders_held, request.tmp_path);
	}
	const fs::path target = reached / request.leaf;
	// FILE_CREATE: nothing planted at the temp name is opened, not even a link.
	handle_holder file = open_in(chain.last(), request.tmp_leaf, FILE_GENERIC_WRITE | DELETE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE);
	if (!file.valid()) {
		return last_error_text("cannot create", request.tmp_path);
	}
	temp_guard guard{ file.get(), request.tmp_path };
	if (!lies_at(file.get(), chain.root_path, *parts, request.tmp_leaf, unnormalized::refuse)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the temp file was not created below the root", request.tmp_path);
	}
	const auto write_piece = [&file](std::span<const std::byte> piece) {
		std::size_t done = 0;
		while (done < piece.size()) {
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(piece.size() - done, 1u << 30));
			DWORD wrote = 0;
			if (!::WriteFile(file.get(), piece.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
				return false;
			}
			done += wrote;
		}
		return true;
	};
	if (request.stream) {
		if (!(*request.stream)(write_piece)) {
			return last_error_text("writing failed for", request.tmp_path);
		}
	} else if (!write_piece(request.data)) {
		return last_error_text("writing failed for", request.tmp_path);
	}
	if (request.durable && !::FlushFileBuffers(file.get())) {
		return last_error_text("cannot flush", request.tmp_path);
	}

	if (request.hook && *request.hook) {
		(*request.hook)(write_stage::before_rename, request.tmp_path);
	}
	if (request.replace && !replaced_leaf_lies_at(chain, *parts, request.leaf)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the file to replace is not below the root under this name", target);
	}
	// A file that was just removed (an erase on this thread) can stay delete-pending while another handle (watcher,
	// indexer, antivirus) is open on it, and replacing it is denied for a moment.
	bool renamed = false;
	for (int attempt = 0; attempt < 50 && !renamed; ++attempt) {
		renamed = rename_to(file.get(), chain.last(), request.leaf, request.replace);
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

std::string platform::beneath::remove_beneath(pinned_root& root, const fs::path& folder, const fs::path& leaf, bool files_only)
{
	const auto parts = folder_parts(folder);
	if (!parts || !is_plain_name(leaf)) {
		return std::string{ k_not_below };
	}
	fs::path reached;
	const held_chain chain = hold_chain(root.path(), *parts, false, reached, unnormalized::refuse);
	if (chain.empty()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
			? std::string{}
			: last_error_text("cannot open the folder (a link, a junction, or not a folder)", reached);
	}
	const fs::path path = reached / leaf;
	handle_holder file = open_in(chain.last(), leaf, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
		files_only ? FILE_NON_DIRECTORY_FILE : 0);
	if (!file.valid()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? std::string{} : last_error_text("cannot remove", path);
	}
	if (files_only) {
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(file.get(), &information)) {
			return last_error_text("cannot inspect", path);
		}
		if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return last_error_text("not a regular file", path);
		}
	}
	// As write_beneath and rename_beneath: a short (8.3) name or any other alias of a folder or of the leaf would
	// delete a file elsewhere than the names say.
	if (!lies_at(file.get(), chain.root_path, *parts, leaf, unnormalized::refuse)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the file was not found below the root under these names", path);
	}
	if (!set_delete_on_close(file.get())) {
		return last_error_text("cannot remove", path);
	}
	return {};
}

namespace {

	// The names in the folder `folder` holds, "." and ".." left out, enumerated through that handle. False with the
	// last error set when it cannot be read.
	bool list_folder(HANDLE folder, std::vector<std::wstring>& names)
	{
		// FILE_ID_BOTH_DIR_INFO entries are 8-byte aligned within the buffer.
		std::vector<std::uint64_t> buffer(64 * 1024 / sizeof(std::uint64_t));
		const auto bytes = reinterpret_cast<const std::byte*>(buffer.data());
		FILE_INFO_BY_HANDLE_CLASS what = FileIdBothDirectoryRestartInfo;
		for (;;) {
			const auto size = static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t));
			if (!::GetFileInformationByHandleEx(folder, what, buffer.data(), size)) {
				return ::GetLastError() == ERROR_NO_MORE_FILES;
			}
			what = FileIdBothDirectoryInfo;
			for (std::size_t offset = 0;;) {
				const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(bytes + offset);
				std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
				if (name != L"." && name != L"..") {
					names.push_back(std::move(name));
				}
				if (entry->NextEntryOffset == 0) {
					break;
				}
				offset += entry->NextEntryOffset;
			}
		}
	}

	// FileDispositionInfoEx and its flags (Windows 10 1607), spelled out: the SDK declares them only for a high enough
	// _WIN32_WINNT.
	constexpr auto k_file_disposition_info_ex = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
	constexpr DWORD k_disposition_delete = 0x1;
	constexpr DWORD k_disposition_posix_semantics = 0x2;
	constexpr DWORD k_disposition_ignore_readonly = 0x10;

	// Marks what `entry` (opened with DELETE) holds for deletion when its handle closes, a read-only file too: with
	// POSIX semantics (the name goes at once, so its folder is empty for the next step) where Windows has them, else the
	// read-only attribute is cleared through a handle opened again from `entry` and the classic disposition set.
	bool delete_entry(HANDLE entry)
	{
		DWORD flags = k_disposition_delete | k_disposition_posix_semantics | k_disposition_ignore_readonly;
		if (::SetFileInformationByHandle(entry, k_file_disposition_info_ex, &flags, sizeof(flags))) {
			return true;
		}
		const DWORD code = ::GetLastError();
		if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED && code != ERROR_INVALID_FUNCTION) {
			return false;
		}
		FILE_BASIC_INFO basic{};
		if (::GetFileInformationByHandleEx(entry, FileBasicInfo, &basic, sizeof(basic))
			&& (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
			basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
			if (basic.FileAttributes == 0) {
				basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
			}
			constexpr ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
			const handle_holder writable = open_in(entry, fs::path{}, FILE_WRITE_ATTRIBUTES, share, FILE_OPEN, 0);
			if (writable.valid()) {
				::SetFileInformationByHandle(writable.get(), FileBasicInfo, &basic, sizeof(basic));
			}
		}
		return set_delete_on_close(entry);
	}

	// The folder `entry` holds, opened again from that handle (no name is looked up) to be listed: as itself for a
	// plain folder, through its filter for a placeholder. Invalid with the last error set unless it is the same folder.
	handle_holder reopen_to_list(HANDLE entry, const BY_HANDLE_FILE_INFORMATION& information, bool through_filter)
	{
		// FILE_SHARE_DELETE: `entry` itself holds DELETE. Nobody else gets it: `entry` does not share it.
		constexpr ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		handle_holder listed = open_in(entry, fs::path{}, k_folder_access, share, FILE_OPEN, FILE_DIRECTORY_FILE,
			!through_filter);
		BY_HANDLE_FILE_INFORMATION after{};
		if (!listed.valid() || !::GetFileInformationByHandle(listed.get(), &after)) {
			return handle_holder{};
		}
		if (!same_file(information, after) || (after.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return handle_holder{};
		}
		return listed;
	}

	// Empties what `name` in `parent` is and removes it. The entry is opened as itself (FILE_OPEN_REPARSE_POINT) with
	// DELETE alone, so a file locked or unreadable elsewhere is still removed, and held without FILE_SHARE_DELETE, so
	// nobody renames or replaces it meanwhile. What is done with it is tree_entry_of: a folder is listed through a
	// handle opened again from it and each entry handled the same way; a link, a junction or an unknown reparse point is
	// deleted as itself, never entered, except at the top of the tree (`top`), where it is refused.
	std::string remove_tree_at(HANDLE parent, const fs::path& name, const fs::path& path,
		const platform::beneath::tree_hook* hook, int depth, bool top)
	{
		if (depth > k_tree_depth_limit) {
			return std::format("'{}' is {} {} folders", fs_paths::utf8_of(path), platform::beneath::k_tree_too_deep,
				k_tree_depth_limit);
		}
		const handle_holder entry = open_in(parent, name, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0);
		if (!entry.valid()) {
			const DWORD code = ::GetLastError();
			const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
			return missing ? std::string{} : last_error_text("cannot open", path);
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(entry.get(), &information)) {
			return last_error_text("cannot inspect", path);
		}
		FILE_ATTRIBUTE_TAG_INFO tag{};
		const bool tag_known = (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
			|| ::GetFileInformationByHandleEx(entry.get(), FileAttributeTagInfo, &tag, sizeof(tag)) != 0;
		const auto what = platform::beneath::tree_entry_of(information.dwFileAttributes, tag_known, tag.ReparseTag, top);
		if (what == platform::beneath::tree_entry::refused) {
			return std::format("'{}' is a link, a junction or another reparse point: only a real folder is removed as "
				"a tree", fs_paths::utf8_of(path));
		}
		if (what == platform::beneath::tree_entry::walk || what == platform::beneath::tree_entry::walk_through_filter) {
			const bool through_filter = what == platform::beneath::tree_entry::walk_through_filter;
			const handle_holder listed = reopen_to_list(entry.get(), information, through_filter);
			if (!listed.valid()) {
				return last_error_text("cannot open the folder to list it", path);
			}
			if (hook && *hook) {
				(*hook)(path);
			}
			std::vector<std::wstring> names;
			if (!list_folder(listed.get(), names)) {
				return last_error_text("cannot list", path);
			}
			for (const auto& child : names) {
				const fs::path child_name{ child };
				std::string error = remove_tree_at(listed.get(), child_name, path / child_name, hook, depth + 1, false);
				if (!error.empty()) {
					return error;
				}
			}
		}
		// A folder whose entries another handle (indexer, antivirus) still holds open is not empty yet for a moment.
		DWORD code = ERROR_SUCCESS;
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (delete_entry(entry.get())) {
				return {};
			}
			code = ::GetLastError();
			if (code != ERROR_DIR_NOT_EMPTY) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		::SetLastError(code);
		return last_error_text("cannot remove", path);
	}

}

std::string platform::beneath::make_folders_beneath(pinned_root& root, const fs::path& folder, bool must_be_new)
{
	auto parts = folder_parts(folder);
	if (!parts || (must_be_new && parts->empty())) {
		return std::string{ k_not_below };
	}
	if (must_be_new) {
		return make_new_folder_beneath(root, folder, 0777);
	}
	fs::path reached;
	const held_chain chain = hold_chain(root.path(), *parts, true, reached, unnormalized::refuse);
	return chain.empty()
		? last_error_text("cannot make the folder (a link, a junction, or not a folder, on the way)", reached)
		: std::string{};
}

std::string platform::beneath::make_new_folder_beneath(pinned_root& root, const fs::path& folder, std::uint32_t mode,
	folder_identity* made)
{
	auto parts = folder_parts(folder);
	if (!parts || parts->empty()) {
		return std::string{ k_not_below };
	}
	fs::path reached;
	// Nothing for group and others: the folder lets the current user alone in, whatever the folders above allow.
	local_descriptor private_security;
	if ((mode & 077) == 0 && !private_descriptor(private_security)) {
		return last_error_text("cannot build a private access list for", folder);
	}
	const fs::path last = parts->back();
	parts->pop_back();
	const held_chain chain = hold_chain(root.path(), *parts, true, reached, unnormalized::refuse);
	if (chain.empty()) {
		return last_error_text("cannot make the folder (a link, a junction, or not a folder, on the way)", reached);
	}
	reached /= last;
	const handle_holder created = open_in(chain.last(), last, k_folder_access, k_folder_share, FILE_CREATE,
		FILE_DIRECTORY_FILE, true, private_security.descriptor);
	if (!created.valid()) {
		const DWORD code = ::GetLastError();
		const bool exists = code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS;
		return exists ? std::format("'{}' already exists", fs_paths::utf8_of(reached))
					  : last_error_text("cannot make", reached);
	}
	if (!lies_at(created.get(), chain.root_path, *parts, last, unnormalized::refuse)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the folder was not made below the root under these names", reached);
	}
	if (made && !identity_of(created.get(), *made)) {
		return last_error_text("cannot inspect the folder just made", reached);
	}
	return {};
}

std::string platform::beneath::remove_tree_beneath_watched(pinned_root& root, const fs::path& folder,
	const fs::path& leaf, const tree_hook& watch)
{
	const tree_hook* const hook = &watch;
	const auto parts = folder_parts(folder);
	if (!parts || !is_plain_name(leaf)) {
		return std::string{ k_not_below };
	}
	fs::path reached;
	const held_chain chain = hold_chain(root.path(), *parts, false, reached, unnormalized::refuse);
	if (chain.empty()) {
		const DWORD code = ::GetLastError();
		return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
			? std::string{}
			: last_error_text("cannot open the folder (a link, a junction, or not a folder)", reached);
	}
	const fs::path path = reached / leaf;
	{
		// As remove_beneath: a short (8.3) name or any other alias would remove a tree elsewhere than the names say.
		constexpr ULONG share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		const handle_holder named = open_in(chain.last(), leaf, 0, share_all, FILE_OPEN, 0);
		if (!named.valid()) {
			const DWORD code = ::GetLastError();
			const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
			return missing ? std::string{} : last_error_text("cannot open", path);
		}
		if (!lies_at(named.get(), chain.root_path, *parts, leaf, unnormalized::refuse)) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return last_error_text("the tree was not found below the root under these names", path);
		}
	}
	return remove_tree_at(chain.last(), leaf, path, hook, 0, true);
}

platform::beneath::list_result platform::beneath::list_beneath(pinned_root& root, const fs::path& folder, std::size_t max_entries)
{
	list_result out;
	const auto parts = folder_parts(folder);
	if (!parts) {
		out.error = std::string{ k_not_below };
		return out;
	}
	fs::path reached;
	const held_chain chain = hold_chain(root.path(), *parts, false, reached, unnormalized::refuse);
	if (chain.empty()) {
		const DWORD code = ::GetLastError();
		if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
			out.missing = true;
		} else {
			out.error = last_error_text("cannot open the folder (a link, a junction, or not a folder)", reached);
		}
		return out;
	}
	// FILE_ID_BOTH_DIR_INFO entries are 8-byte aligned within the buffer.
	std::vector<std::uint64_t> buffer(64 * 1024 / sizeof(std::uint64_t));
	const auto bytes = reinterpret_cast<const std::byte*>(buffer.data());
	FILE_INFO_BY_HANDLE_CLASS what = FileIdBothDirectoryRestartInfo;
	for (;;) {
		const auto size = static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t));
		if (!::GetFileInformationByHandleEx(chain.last(), what, buffer.data(), size)) {
			if (::GetLastError() != ERROR_NO_MORE_FILES) {
				out.error = last_error_text("cannot list", reached);
				out.entries.clear();
			}
			return out;
		}
		what = FileIdBothDirectoryInfo;
		for (std::size_t offset = 0;;) {
			const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(bytes + offset);
			std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
			if (name != L"." && name != L"..") {
				if (out.entries.size() >= max_entries) {
					out.truncated = true;
					return out;
				}
				listed_entry item;
				item.name = fs::path(std::move(name));
				// Any reparse point is a link here, a cloud placeholder too: nothing is entered through one.
				if ((entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
					item.kind = entry_kind::link;
				} else if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
					item.kind = entry_kind::folder;
				} else if ((entry->FileAttributes & FILE_ATTRIBUTE_DEVICE) != 0) {
					item.kind = entry_kind::other;
				} else {
					item.kind = entry_kind::file;
				}
				out.entries.push_back(std::move(item));
			}
			if (entry->NextEntryOffset == 0) {
				break;
			}
			offset += entry->NextEntryOffset;
		}
	}
}

std::string platform::beneath::rename_beneath(pinned_root& root, const rename_request& request)
{
	if (request.across_volumes) {
		*request.across_volumes = false;
	}
	const auto from_parts = folder_parts(request.from_folder);
	const auto to_parts = folder_parts(request.to_folder);
	if (!from_parts || !to_parts || !is_plain_name(request.from_leaf) || !is_plain_name(request.to_leaf)) {
		return std::string{ k_not_below };
	}
	fs::path from_reached;
	const held_chain from_chain = hold_chain(root.path(), *from_parts, false, from_reached, unnormalized::refuse);
	if (from_chain.empty()) {
		return last_error_text("cannot open the folder (a link, a junction, or not a folder)", from_reached);
	}
	const fs::path source = from_reached / request.from_leaf;
	handle_holder file = open_in(from_chain.last(), request.from_leaf, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
	if (!lies_at(file.get(), from_chain.root_path, *from_parts, request.from_leaf, unnormalized::refuse)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the file was not found below the root", source);
	}
	fs::path to_reached;
	const held_chain to_chain = hold_chain(root.path(), *to_parts, false, to_reached, unnormalized::refuse);
	if (to_chain.empty()) {
		return last_error_text("cannot open the target folder (missing, a link, a junction, or not a folder)", to_reached);
	}
	if (request.allow_replace && !replaced_leaf_lies_at(to_chain, *to_parts, request.to_leaf)) {
		::SetLastError(ERROR_CANT_ACCESS_FILE);
		return last_error_text("the file to replace is not below the root under this name", to_reached / request.to_leaf);
	}
	if (!rename_to(file.get(), to_chain.last(), request.to_leaf, request.allow_replace)) {
		const DWORD code = ::GetLastError();
		if (request.across_volumes) {
			*request.across_volumes = code == ERROR_NOT_SAME_DEVICE;
		}
		::SetLastError(code);
		return last_error_text("cannot move to", to_reached / request.to_leaf);
	}
	return {};
}

namespace {
	// FILETIME and FILE_BASIC_INFO count 100 ns ticks since 1601-01-01.
	std::int64_t unix_nanoseconds_of(std::int64_t ticks)
	{
		constexpr std::int64_t k_ticks_to_unix_epoch = 116'444'736'000'000'000;
		return (ticks - k_ticks_to_unix_epoch) * 100;
	}

	// nullopt when the change time cannot be read: the read goes on without a stamp.
	std::optional<platform::beneath::file_stamp> stamp_of(HANDLE file, const BY_HANDLE_FILE_INFORMATION& information)
	{
		FILE_BASIC_INFO basic{};
		if (!::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) {
			return std::nullopt;
		}
		const std::int64_t written = static_cast<std::int64_t>(
			(static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) | information.ftLastWriteTime.dwLowDateTime);
		return platform::beneath::file_stamp{ (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow,
			unix_nanoseconds_of(written), unix_nanoseconds_of(basic.ChangeTime.QuadPart),
			(static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow,
			static_cast<std::uint64_t>(information.dwVolumeSerialNumber) };
	}

	// The stamp taken at the open, if the same handle still shows it and `read_bytes` were read: else the file changed
	// while it was read.
	std::optional<platform::beneath::file_stamp> stamp_after_read(HANDLE file,
		const std::optional<platform::beneath::file_stamp>& opened, std::uint64_t read_bytes)
	{
		BY_HANDLE_FILE_INFORMATION information{};
		if (!opened || read_bytes != opened->size || !::GetFileInformationByHandle(file, &information)
			|| stamp_of(file, information) != opened) {
			return std::nullopt;
		}
		return opened;
	}

	// What read_beneath and read_beneath_stream share: the file of `request` opened as read_beneath describes; `file_out`,
	// `size_out`, `stamp_out` and `path_out` are set when the result is ok.
	platform::beneath::read_result open_for_read(const platform::beneath::read_request& request, handle_holder& file_out,
		std::uint64_t& size_out, std::optional<platform::beneath::file_stamp>& stamp_out, fs::path& path_out)
	{
		using namespace platform::beneath;
		const auto parts = folder_parts(request.folder);
		if (!parts || !is_plain_name(request.leaf)) {
			return { read_status::failed, std::string{ k_not_below } };
		}
		if (request.hook && *request.hook) {
			(*request.hook)(read_stage::before_walk);
		}
		const auto is_missing = [] {
			const DWORD code = ::GetLastError();
			return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
		};
		fs::path reached;
		const held_chain chain = hold_chain(request.root, *parts, false, reached, unnormalized::check_names);
		if (chain.empty()) {
			return { is_missing() ? read_status::missing : read_status::failed,
				last_error_text("cannot open the folder (a link, a junction, or not a folder)", reached) };
		}
		if (request.hook && *request.hook) {
			(*request.hook)(read_stage::folders_held);
		}
		const fs::path path = reached / request.leaf;
		// Opened itself if it is a reparse point (open_in adds FILE_OPEN_REPARSE_POINT); shared, so a writer may still
		// replace or remove the file while it is read.
		constexpr ULONG k_file_share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		file_out = open_in(chain.last(), request.leaf, FILE_READ_DATA, k_file_share, FILE_OPEN, 0);
		if (!file_out.valid()) {
			if (is_missing()) {
				return { read_status::missing, {} };
			}
			return { read_status::failed, last_error_text("cannot open", path) };
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(file_out.get(), &information)) {
			return { read_status::failed, last_error_text("cannot inspect", path) };
		}
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			return { read_status::not_regular, {} };
		}
		switch (reparse_of(file_out.get(), information)) {
		case reparse_kind::none:
			break;
		case reparse_kind::redirect:
			return { read_status::not_regular, {} };
		case reparse_kind::unknown:
			return { read_status::failed, last_error_text("cannot inspect the reparse point", path) };
		case reparse_kind::filter: {
			// A placeholder or a deduplicated file: read through its filter, if that is still the same file.
			handle_holder followed = open_in(chain.last(), request.leaf, FILE_READ_DATA, k_file_share, FILE_OPEN,
				FILE_NON_DIRECTORY_FILE, false);
			BY_HANDLE_FILE_INFORMATION after{};
			if (!followed.valid() || !::GetFileInformationByHandle(followed.get(), &after)) {
				return { read_status::failed, last_error_text("cannot open through its file system filter", path) };
			}
			if (!same_file(information, after)) {
				return { read_status::not_regular, {} };
			}
			file_out = std::move(followed);
			information = after;
			break;
		}
		}
		if (::GetFileType(file_out.get()) != FILE_TYPE_DISK) {
			return { read_status::not_regular, {} };
		}
		// A short (8.3) name or any other alias would read a file elsewhere than the names say.
		if (!lies_at(file_out.get(), chain.root_path, *parts, request.leaf, unnormalized::check_names)) {
			::SetLastError(ERROR_CANT_ACCESS_FILE);
			return { read_status::failed, last_error_text("the file was not found below the root under these names", path) };
		}
		const std::uint64_t size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
		if (size > request.max_bytes) {
			return { read_status::too_large, {} };
		}
		size_out = size;
		stamp_out = stamp_of(file_out.get(), information);
		path_out = path;
		return { read_status::ok, {} };
	}
}

platform::beneath::read_result platform::beneath::read_beneath(const read_request& request, std::vector<std::byte>& out)
{
	out.clear();
	handle_holder file;
	std::uint64_t size = 0;
	std::optional<file_stamp> stamp;
	fs::path path;
	if (read_result opened = open_for_read(request, file, size, stamp, path); opened.status != read_status::ok) {
		return opened;
	}
	if (request.stamp_only) {
		read_result result{ read_status::ok, {} };
		result.stamp = stamp;
		return result;
	}
	out.resize(static_cast<std::size_t>(size));
	std::size_t done = 0;
	while (done < out.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 30));
		DWORD got = 0;
		if (!::ReadFile(file.get(), out.data() + done, chunk, &got, nullptr)) {
			out.clear();
			return { read_status::failed, last_error_text("cannot read", path) };
		}
		if (got == 0) {
			break; // shortened since the size was taken: what is there now
		}
		done += got;
	}
	out.resize(done);
	read_result result{ read_status::ok, {} };
	result.stamp = stamp_after_read(file.get(), stamp, done);
	return result;
}

platform::beneath::read_result platform::beneath::read_beneath_stream(const read_request& request,
	const std::function<bool(std::span<const std::byte>)>& sink)
{
	handle_holder file;
	std::uint64_t size = 0;
	std::optional<file_stamp> stamp;
	fs::path path;
	if (read_result opened = open_for_read(request, file, size, stamp, path); opened.status != read_status::ok) {
		return opened;
	}
	std::vector<std::byte> buffer(64 * 1024);
	std::uint64_t total = 0;
	for (;;) {
		DWORD got = 0;
		if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
			return { read_status::failed, last_error_text("cannot read", path) };
		}
		if (got == 0) {
			break;
		}
		total += static_cast<std::uint64_t>(got);
		if (total > request.max_bytes) {
			return { read_status::too_large, {} };
		}
		if (!sink(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(got)))) {
			break;
		}
	}
	read_result result{ read_status::ok, {} };
	result.stamp = stamp_after_read(file.get(), stamp, total);
	return result;
}

fs::path platform::beneath::long_path(const fs::path& path)
{
	const std::wstring& text = path.native();
	if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) {
		return path;
	}
	std::error_code ec;
	const std::wstring whole = fs::absolute(path, ec).lexically_normal().make_preferred().native();
	if (ec) {
		return path;
	}
	return fs::path{ whole.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + whole.substr(2) : L"\\\\?\\" + whole };
}

std::optional<platform::beneath::link_entry> platform::beneath::link_entry_at(const fs::path& path, std::error_code& ec)
{
	ec.clear();
	const handle_holder entry{ ::CreateFileW(long_path(path).c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
	if (!entry.valid()) {
		const DWORD code = ::GetLastError();
		if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
			ec = std::error_code(static_cast<int>(code), std::system_category());
		}
		return std::nullopt;
	}
	FILE_ATTRIBUTE_TAG_INFO information{};
	if (!::GetFileInformationByHandleEx(entry.get(), FileAttributeTagInfo, &information, sizeof(information))) {
		ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
		return std::nullopt;
	}
	const bool reparse_point = (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	return link_entry_of(information.FileAttributes, true, reparse_point ? information.ReparseTag : 0);
}

struct platform::beneath::held_folder::state {
	std::vector<fs::path> names;
	held_chain chain;
	std::vector<handle_holder> files;
	std::vector<std::wstring> final_paths;
};

platform::beneath::held_folder::held_folder(std::unique_ptr<state> held)
	: m_state(std::move(held))
{
}

platform::beneath::held_folder::~held_folder() = default;

const std::vector<fs::path>& platform::beneath::held_folder::names() const
{
	return m_state->names;
}

fs::path platform::beneath::held_folder::load_path(const fs::path& name) const
{
	const auto index = held_index(m_state->names, name);
	return index ? fs::path{ m_state->final_paths[*index] } : fs::path{};
}

bool platform::beneath::held_folder::read(const fs::path& name, const chunk_sink& sink) const
{
	const auto index = held_index(m_state->names, name);
	if (!index) {
		return false;
	}
	const HANDLE file = m_state->files[*index].get();
	LARGE_INTEGER start{};
	if (!::SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
		return false;
	}
	std::vector<std::byte> buffer(64 * 1024);
	for (;;) {
		DWORD got = 0;
		if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
			return false;
		}
		if (got == 0) {
			return true;
		}
		if (!sink(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(got)))) {
			return false;
		}
	}
}

platform::beneath::hold_result platform::beneath::hold_folder_beneath(pinned_root& root, const fs::path& folder,
	const folder_identity& made, std::span<const fs::path> names)
{
	const auto parts = folder_parts(folder);
	if (!parts || parts->empty() || !std::ranges::all_of(names, is_plain_name)) {
		return hold_refused(hold_status::failed, std::string{ k_not_below });
	}
	fs::path reached;
	auto kept = std::make_unique<held_folder::state>();
	kept->names.assign(names.begin(), names.end());
	kept->chain = hold_chain(root.path(), *parts, false, reached, unnormalized::refuse);
	if (kept->chain.empty()) {
		return hold_refused(hold_status::failed,
			last_error_text("cannot open the folder (a link, a junction, or not a folder)", reached));
	}
	{
		// Its permissions are read through a handle of its own (the chain holds it without READ_CONTROL), checked to
		// be the folder held: the Windows side of the POSIX owner and mode check.
		const HANDLE parent = kept->chain.folders[kept->chain.folders.size() - 2].get();
		const handle_holder inspected = open_in(parent, parts->back(), READ_CONTROL,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, FILE_DIRECTORY_FILE);
		BY_HANDLE_FILE_INFORMATION held_information{};
		BY_HANDLE_FILE_INFORMATION inspected_information{};
		if (!inspected.valid() || !::GetFileInformationByHandle(kept->chain.last(), &held_information)
			|| !::GetFileInformationByHandle(inspected.get(), &inspected_information)
			|| !same_file(held_information, inspected_information)) {
			return hold_refused(hold_status::failed, last_error_text("cannot read the permissions of", reached));
		}
		folder_identity held_identity;
		if (!identity_of(kept->chain.last(), held_identity)) {
			return hold_refused(hold_status::failed, last_error_text("cannot inspect", reached));
		}
		if (held_identity != made) {
			return hold_refused(hold_status::failed, std::format("'{}' is not the folder made for the copy",
				fs_paths::utf8_of(reached)));
		}
		if (std::string why = not_private_reason(inspected.get()); !why.empty()) {
			return hold_refused(hold_status::not_private, std::format("'{}' is not a folder of this user closed to "
				"others: {}", fs_paths::utf8_of(reached), why));
		}
	}
	{
		// The loader looks for a library's dependencies in its folder first: nothing may be there but the copy.
		std::vector<std::wstring> present;
		if (!list_folder(kept->chain.last(), present)) {
			return hold_refused(hold_status::failed, last_error_text("cannot list", reached));
		}
		for (const auto& entry : present) {
			if (std::ranges::find(names, fs::path{ entry }) == names.end()) {
				return hold_refused(hold_status::failed, std::format("'{}' holds '{}', which was not put there",
					fs_paths::utf8_of(reached), fs_paths::utf8_of(fs::path{ entry })));
			}
		}
	}
	for (const auto& name : names) {
		const fs::path path = reached / name;
		// Sharing reading alone: refused while anyone has it open for writing or deleting, and nobody can while held.
		handle_holder file = open_in(kept->chain.last(), name, FILE_GENERIC_READ, FILE_SHARE_READ, FILE_OPEN,
			FILE_NON_DIRECTORY_FILE);
		if (!file.valid()) {
			return hold_refused(hold_status::failed,
				last_error_text("cannot hold for reading alone (open for writing elsewhere, or missing)", path));
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!::GetFileInformationByHandle(file.get(), &information)) {
			return hold_refused(hold_status::failed, last_error_text("cannot inspect", path));
		}
		if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
			return hold_refused(hold_status::failed,
				std::format("'{}' is a link, a reparse point or a folder", fs_paths::utf8_of(path)));
		}
		if (!lies_at(file.get(), kept->chain.root_path, *parts, name, unnormalized::refuse)) {
			return hold_refused(hold_status::failed,
				std::format("'{}' does not lie at its name", fs_paths::utf8_of(path)));
		}
		const auto held_path = final_path(file.get());
		if (!held_path) {
			return hold_refused(hold_status::failed, last_error_text("no final path of", path));
		}
		kept->final_paths.push_back(*held_path);
		kept->files.push_back(std::move(file));
	}
	hold_result result;
	result.status = hold_status::held;
	result.held = std::make_shared<held_folder>(std::move(kept));
	call_hold_hook(folder);
	return result;
}

#endif
