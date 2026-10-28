#pragma once
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

// How vfs_resolver writes and removes files under a mounted root without a race between the path check and the file
// operation (#414): every directory below the root is opened in turn without following a link (POSIX: openat with
// O_NOFOLLOW from a root descriptor kept open; Windows: a chain of directory handles, each refusing a reparse point and
// opened without FILE_SHARE_DELETE, so none can be renamed or replaced while the write runs), the temp file is created
// only if nothing is there yet, and the rename happens inside the directory that was opened. Internal to the resolver.
namespace res::detail {

	using write_hook = std::function<void(write_stage, const std::filesystem::path& tmp_path)>;

	// The root a resolver writes under. POSIX: its descriptor, opened on the first write (the root may not exist when it
	// is mounted) and opened again when the directory it names was removed. Windows: nothing kept, the chain of handles
	// is opened per write, so the root's folder stays movable while the editor runs. Only the io thread uses it.
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
		std::string leaf;
		std::string tmp_leaf;
		std::span<const std::byte> data;
		// fsync of the file before the rename and of the folder after it (#417).
		bool durable = true;
		// Tests only: called just before the rename, with the temp file's full path.
		const write_hook* hook = nullptr;
		std::filesystem::path tmp_path;
	};

	// Empty on success, else why it failed; on failure the target is as it was and the temp file is gone.
	std::string write_beneath(pinned_root& root, const write_request& request);
	// Empty on success (also when there is nothing to remove), else why it failed.
	std::string remove_beneath(pinned_root& root, const std::filesystem::path& folder, std::string_view leaf);

}
