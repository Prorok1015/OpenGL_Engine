#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <snk/common/snk_api.h>

namespace platform
{
	// An exclusive advisory lock of the operating system on a file (POSIX: flock; Windows: LockFileEx), held until the
	// object goes, and released by the system when the process dies, so a crash leaves nothing to clean up. Another
	// holder, in this process or another one, does not get it.
	class SNK_API file_lock
	{
	public:
		// The lock on <root>/<relative> (made, with its folders, when missing); none when another holder has it, or
		// `error` (may be null) says why the file could not be opened. `relative` is a chain of plain names. Below the
		// root nothing is followed: a link (or a junction) at a folder or at the file is an error and its target stays
		// untouched, so a project cannot point the lock at a file outside it. The root itself may be a link.
		static std::optional<file_lock> try_acquire(const std::filesystem::path& root, const std::filesystem::path& relative,
			std::string* error = nullptr);

		file_lock(file_lock&& other) noexcept;
		file_lock& operator=(file_lock&& other) noexcept;
		file_lock(const file_lock&) = delete;
		file_lock& operator=(const file_lock&) = delete;
		~file_lock();

	private:
		explicit file_lock(std::intptr_t handle) : m_handle(handle) {}
		void release() noexcept;

		std::intptr_t m_handle = -1;
	};
}
