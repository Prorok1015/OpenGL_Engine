#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <snk/common/snk_api.h>

namespace platform
{
	struct lock_attempt;

	// What an attempt found (#1187). `held`: another holder has it. `unsupported`: the file system takes no lock there
	// (POSIX ENOLCK/ENOTSUP/EOPNOTSUPP, Windows ERROR_NOT_SUPPORTED/ERROR_INVALID_FUNCTION), so nothing says whether a
	// holder is alive. `missing`: an existing_only attempt found no file.
	enum class lock_outcome : std::uint8_t { acquired, held, missing, unsupported, failed };

	// existing_only makes nothing, neither the file nor a folder nor the root: a probe of another holder's lock file.
	enum class lock_open : std::uint8_t { create, existing_only };

	// An exclusive advisory lock of the operating system on a file (POSIX: flock; Windows: LockFileEx), held until the
	// object goes, and released by the system when the process dies, so a crash leaves nothing to clean up. Another
	// holder, in this process or another one, does not get it.
	class SNK_API file_lock
	{
	public:
		// The lock on <root>/<relative>, or what kept it. `relative` is a chain of plain names. Below the root nothing is
		// followed: a link (or a junction) at a folder or at the file is `failed` and its target stays untouched, so a
		// project cannot point the lock at a file outside it. The root itself may be a link. Once locked, the file is
		// checked to still be at that name: a lock on a file removed meanwhile would hold nothing.
		static lock_attempt acquire(const std::filesystem::path& root, const std::filesystem::path& relative,
			lock_open open = lock_open::create);

		// acquire with lock_open::create (the file and its folders made when missing): none when another holder has it
		// (`error` stays empty), or `error` (may be null) says why it could not be taken.
		static std::optional<file_lock> try_acquire(const std::filesystem::path& root, const std::filesystem::path& relative,
			std::string* error = nullptr);

		file_lock(file_lock&& other) noexcept;
		file_lock& operator=(file_lock&& other) noexcept;
		file_lock(const file_lock&) = delete;
		file_lock& operator=(const file_lock&) = delete;
		~file_lock();

	private:
		explicit file_lock(std::intptr_t handle) : m_handle(handle) {}
		static lock_attempt acquire_once(const std::filesystem::path& root, const std::vector<std::filesystem::path>& parts,
			bool create, bool& gone);
		void release() noexcept;

		std::intptr_t m_handle = -1;
	};

	struct lock_attempt {
		lock_outcome outcome = lock_outcome::failed;
		// Set for `acquired` only.
		std::optional<file_lock> lock;
		// Why, for `unsupported` and `failed`.
		std::string error;
	};
}
