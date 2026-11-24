#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <snk/common/snk_api.h>

// How files are written, removed and read under a root folder without a race between the path check and the file
// operation (#414, #484; moved here from the resource system for #523): every folder below the root is opened relative
// to the one above it without following a link (POSIX: openat with O_NOFOLLOW from a root descriptor kept open;
// Windows: NtCreateFile relative to the parent's handle with FILE_OPEN_REPARSE_POINT, each refusing a link or a junction,
// a folder a filter keeps in place such as a OneDrive placeholder entered through the filter (folder_entry_of in
// plt_reparse_tag.h, #523), and held without FILE_SHARE_DELETE), the temp file is created relative to the last folder only if nothing is there yet,
// and the rename happens inside that folder.
namespace platform::beneath {

	// Where a write may be paused by a test hook: after the path was checked again on the io thread, and with the temp
	// file written, before the rename.
	enum class write_stage : int { after_check, folders_held, before_rename };
	// Where a read may be paused the same way (#484): on the io thread before the folders are opened, and with them held
	// before the file is opened.
	enum class read_stage : int { before_walk, folders_held };

	using write_hook = std::function<void(write_stage, const std::filesystem::path& tmp_path)>;
	// Takes the next piece of a file being written; false: it could not be written, stop.
	using chunk_sink = std::function<bool(std::span<const std::byte>)>;
	// Hands the whole content to `sink` in pieces, in order. false: the content could not be produced (or `sink` refused a
	// piece): the temp file is removed and the target stays as it was. It may check the content once it is complete and
	// answer false then, so nothing is flushed or moved into place.
	using chunk_source = std::function<bool(const chunk_sink& sink)>;

	// The root a resolver writes under. POSIX: its descriptor, opened on the first write (the root may not exist when it
	// is mounted) and opened again when the folder at the root's path is no longer the one it holds (removed, or renamed
	// and replaced). Windows: nothing kept, the chain of handles is opened per write, so the root's folder stays movable
	// while the editor runs. Only the io thread uses it.
	class SNK_API pinned_root
	{
	public:
		explicit pinned_root(std::filesystem::path root) : m_root(std::move(root)) {}
		~pinned_root();
		pinned_root(const pinned_root&) = delete;
		pinned_root& operator=(const pinned_root&) = delete;

		const std::filesystem::path& path() const { return m_root; }
#if !defined(_WIN32)
		// The root's descriptor, created as a folder if missing; -1 and `error` set when it cannot be opened.
		int descriptor(std::string& error);
#endif

	private:
		std::filesystem::path m_root;
#if !defined(_WIN32)
		int m_fd = -1;
#endif
	};

	struct write_request {
		// Below the root, as safe_path resolved it: canonical, so every component was a real folder when checked.
		std::filesystem::path folder;
		// Single names inside `folder`.
		std::filesystem::path leaf;
		std::filesystem::path tmp_leaf;
		std::span<const std::byte> data;
		// fsync of the file before the rename and of the folder after it (#417).
		bool durable = true;
		// false: the temp file is moved to the target only if nothing is there yet (a new file that must not replace one
		// an IDE has just made); an existing target is an error and the temp file is gone.
		bool replace = true;
		// Tests only: called once the folders are held and just before the rename, with the temp file's full path.
		const write_hook* hook = nullptr;
		std::filesystem::path tmp_path;
		// Not null: the content comes from it in pieces (a file too big to hold whole) and `data` is not used.
		const chunk_source* stream = nullptr;
	};

	// Empty on success, else why it failed; on failure the target is as it was and the temp file is gone.
	SNK_API std::string write_beneath(pinned_root& root, const write_request& request);
	// Empty on success (also when there is nothing to remove), else why it failed. An empty folder is removed too,
	// unless `files_only`: then a folder (or anything but a file) is an error and stays.
	SNK_API std::string remove_beneath(pinned_root& root, const std::filesystem::path& folder, const std::filesystem::path& leaf,
		bool files_only = false);

	struct rename_request {
		// Below the root, as safe_path resolved them: canonical folders and single names inside them.
		std::filesystem::path from_folder;
		std::filesystem::path from_leaf;
		std::filesystem::path to_folder;
		std::filesystem::path to_leaf;
		// The target is the source under another case (a case-only rename on a case-insensitive file system), so it may
		// be "replaced" by the source itself.
		bool allow_replace = false;
		// Not null: set to true when the move failed only because the two folders are on different volumes (POSIX EXDEV,
		// Windows ERROR_NOT_SAME_DEVICE), false for every other outcome: a refused link, a missing folder, an existing
		// target, a denied access are not that, and a caller that copies instead must copy for this one alone.
		bool* across_volumes = nullptr;
	};

	// Moves a regular file between folders below the root, each opened without following a link and the move done
	// relative to them. Never replaces: an existing target is an error (POSIX Linux: renameat2 RENAME_NOREPLACE, where
	// the file system lacks it a check first; macOS: renameatx_np RENAME_EXCL; Windows: a rename without ReplaceIfExists).
	// The source is checked first and the target's folders must exist: nothing is made. Empty on success, else why it
	// failed; on failure the file stays where it was.
	SNK_API std::string rename_beneath(pinned_root& root, const rename_request& request);

	// Makes `folder` below the root with every missing folder on the way, each made or opened from the one above it
	// without following a link (Windows: no link or junction): one on the way, or anything but a folder, is an error.
	// `must_be_new`: the last folder must not exist yet, so a name is taken exclusively. Empty on success, else why it
	// failed (the folders made before the failure stay).
	SNK_API std::string make_folders_beneath(pinned_root& root, const std::filesystem::path& folder,
		bool must_be_new = false);

	// make_folders_beneath(root, folder, true) with the last folder made with `mode` (#1304): POSIX mkdirat(mode), the
	// umask only narrows it, so a folder made 0700 is closed to others from its first moment. Windows: a mode with
	// nothing for group and others makes it owned by the current user with a protected DACL granting that user alone
	// (inherited by what is made inside), so nobody else can add a file to it or open one; other modes are not used.
	SNK_API std::string make_new_folder_beneath(pinned_root& root, const std::filesystem::path& folder,
		std::uint32_t mode);

	// Removes `leaf` in `folder` below the root and everything in it (#1294). The way to `folder` is opened without
	// following a link and `leaf` must not be one; inside the tree every folder is opened from the one above it as
	// itself, so a link or a junction met there is removed as the link, never entered. Read-only files go too. Nothing
	// there: success. Empty on success, else why it failed; what was removed before the failure stays removed.
	SNK_API std::string remove_tree_beneath(pinned_root& root, const std::filesystem::path& folder,
		const std::filesystem::path& leaf);

	enum class read_status : std::uint8_t { ok, missing, not_regular, too_large, failed };

	// A file as its open handle saw it. The change time cannot be set back by a tool, unlike the write time.
	struct file_stamp {
		std::uint64_t size = 0;
		// Nanoseconds since the Unix epoch.
		std::int64_t write_time_ns = 0;
		// Of the file's metadata (POSIX ctime, Windows ChangeTime), nanoseconds since the Unix epoch.
		std::int64_t change_time_ns = 0;
		// POSIX inode and device; Windows file index and volume serial number.
		std::uint64_t file_id = 0;
		std::uint64_t device = 0;

		bool operator==(const file_stamp&) const = default;
	};

	struct read_result {
		read_status status = read_status::failed;
		// Why, for `failed` (and the cause of `missing`); empty otherwise.
		std::string error;
		// For `ok` only, and only when the file did not change while it was read (the same handle checked after the read).
		std::optional<file_stamp> stamp{};
	};

	struct read_request {
		// A mounted root, canonical as pinned when mounted. Opened only: never made, so any layer of a mount reads.
		std::filesystem::path root;
		// Below the root, as safe_path resolved it, and a single name inside it.
		std::filesystem::path folder;
		std::filesystem::path leaf;
		// A larger file is `too_large` and nothing is read.
		std::uint64_t max_bytes = 0;
		// Tests only: called before the folders are opened and once they are held, just before the file is opened.
		const std::function<void(read_stage)>* hook = nullptr;
		// read_beneath only: the checks and the stamp, nothing read.
		bool stamp_only = false;
	};

	// Reads a regular file below the root like the writes reach theirs (#484): every folder opened from the one above it
	// without following a link (Windows: no link or junction), the file opened from the last folder without
	// following a link and without blocking (POSIX: O_NONBLOCK, so a FIFO cannot hold the io thread), then checked to be
	// a regular file through that same descriptor or handle and read from it. A link, a folder, a FIFO or a device at
	// the name is `not_regular`.
	SNK_API read_result read_beneath(const read_request& request, std::vector<std::byte>& out);

	// read_beneath without holding the whole file: the same checks, then `sink` gets the file in chunks, in order, and
	// stops the read by returning false (the result is ok then). A file that grows past `max_bytes` while it is read is
	// `too_large` too. The folders are let go once the file is open; the open file is what is read. For a hash of any size.
	SNK_API read_result read_beneath_stream(const read_request& request, const std::function<bool(std::span<const std::byte>)>& sink);

	// Whether `name` could be a Windows short (8.3) alias: every alias holds '~' and a digit. Windows checks each name
	// by the final path; when the file system gives none (some network redirectors), a read goes on only if no name of
	// its path is such a one, and a write or a delete is refused.
	SNK_API bool may_be_short_name(const std::filesystem::path& name);

	// A folder below the root and files in it, held from one walk so that what is read through the hold is what a
	// loader opens next (#1304). POSIX: the folder's descriptor, opened from the root without following a link, of this
	// user and closed to others, holding nothing but the files; each file opened from it without following a link, a
	// regular file of this user, made readable by this user only (0400). Windows: the chain of folders held without
	// FILE_SHARE_DELETE, the folder owned by the current user with a protected DACL that allows nobody else, and each
	// file opened as itself (a reparse point refused) for reading, sharing reading only: while it is held nobody
	// writes, renames or removes one. Destroying it lets all of that go.
	class SNK_API held_folder
	{
	public:
		struct state;
		explicit held_folder(std::unique_ptr<state> held);
		~held_folder();
		held_folder(const held_folder&) = delete;
		held_folder& operator=(const held_folder&) = delete;

		// The files held, in the order hold_folder_beneath was given them.
		const std::vector<std::filesystem::path>& names() const;
		// The path that reaches the held file `name`: Linux /proc/self/fd/<folder>/<name> when it leads to that file;
		// Windows the final path of its handle. Empty when there is none (macOS, a Linux without a usable /proc).
		std::filesystem::path load_path(const std::filesystem::path& name) const;
		// The held file `name` from its start, read through the held descriptor or handle and handed to `sink` in
		// pieces; false when it cannot be read or `sink` refused a piece.
		bool read(const std::filesystem::path& name, const chunk_sink& sink) const;

	private:
		std::unique_ptr<state> m_state;
	};

	enum class hold_status : std::uint8_t { held, not_private, failed };

	struct hold_result {
		hold_status status = hold_status::failed;
		// Set for `held` only.
		std::shared_ptr<held_folder> held;
		// Why not, otherwise.
		std::string error;
	};

	// Holds `folder` below the root and the files `names` (single names) in it. One missing, a link or not a regular
	// file is `failed`; so is, on POSIX, anything else in the folder. POSIX: the folder or a file not of this user, or
	// open to others once made 0400 (a file system that keeps no modes), is `not_private`; Windows: a folder not owned
	// by the current user, with an inherited DACL or one that allows anyone else (no ACLs there), is `not_private`.
	SNK_API hold_result hold_folder_beneath(pinned_root& root, const std::filesystem::path& folder,
		std::span<const std::filesystem::path> names);

}
