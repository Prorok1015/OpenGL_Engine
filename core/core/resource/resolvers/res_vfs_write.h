#pragma once
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

// How vfs_resolver writes and removes files under a mounted root without a race between the path check and the file
// operation (#414): every folder below the root is opened relative to the one above it without following a link
// (POSIX: openat with O_NOFOLLOW from a root descriptor kept open; Windows: NtCreateFile relative to the parent's handle
// with FILE_OPEN_REPARSE_POINT, each refusing a reparse point and held without FILE_SHARE_DELETE), the temp file is
// created relative to the last folder only if nothing is there yet, and the rename happens inside that folder. Internal
// to the resolver.
namespace res::detail {

	using write_hook = std::function<void(write_stage, const std::filesystem::path& tmp_path)>;

	// The root a resolver writes under. POSIX: its descriptor, opened on the first write (the root may not exist when it
	// is mounted) and opened again when the folder at the root's path is no longer the one it holds (removed, or renamed
	// and replaced). Windows: nothing kept, the chain of handles is opened per write, so the root's folder stays movable
	// while the editor runs. Only the io thread uses it.
	class pinned_root
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
	};

	// Empty on success, else why it failed; on failure the target is as it was and the temp file is gone.
	std::string write_beneath(pinned_root& root, const write_request& request);
	// Empty on success (also when there is nothing to remove), else why it failed. An empty folder is removed too,
	// unless `files_only`: then a folder (or anything but a file) is an error and stays.
	std::string remove_beneath(pinned_root& root, const std::filesystem::path& folder, const std::filesystem::path& leaf,
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
	};

	// Moves a regular file between folders below the root, each opened without following a link and the move done
	// relative to them. Never replaces: an existing target is an error (POSIX Linux: renameat2 RENAME_NOREPLACE, where
	// the file system lacks it a check first; macOS: renameatx_np RENAME_EXCL; Windows: a rename without ReplaceIfExists).
	// The source is checked first and the target's folders must exist: nothing is made. Empty on success, else why it
	// failed; on failure the file stays where it was.
	std::string rename_beneath(pinned_root& root, const rename_request& request);

}
