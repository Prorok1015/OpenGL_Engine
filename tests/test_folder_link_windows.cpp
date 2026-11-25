#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "test_folder_link.hpp"
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>

namespace test_util {

	// `link` made a junction (IO_REPARSE_TAG_MOUNT_POINT) to `target` by FSCTL_SET_REPARSE_POINT: no shell, no
	// privilege. The layout is REPARSE_DATA_BUFFER's MountPointReparseBuffer (ntifs.h, not in windows.h).
	bool make_junction(const std::filesystem::path& target, const std::filesystem::path& link)
	{
		const std::wstring print = std::filesystem::absolute(target).lexically_normal().make_preferred().wstring();
		const std::wstring substitute = L"\\??\\" + print;
		constexpr std::size_t header = 8;	// ReparseTag, ReparseDataLength, Reserved
		constexpr std::size_t names_header = 8;	// the offsets and lengths of the two names
		const std::size_t names_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
		std::vector<std::byte> buffer(header + names_header + names_bytes);
		const auto put16 = [&](std::size_t at, std::size_t value) {
			const WORD word = static_cast<WORD>(value);
			std::memcpy(buffer.data() + at, &word, sizeof(word));
		};
		const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
		std::memcpy(buffer.data(), &tag, sizeof(tag));
		put16(4, names_header + names_bytes);
		put16(8, 0);
		put16(10, substitute.size() * sizeof(wchar_t));
		put16(12, (substitute.size() + 1) * sizeof(wchar_t));
		put16(14, print.size() * sizeof(wchar_t));
		const std::size_t substitute_bytes = (substitute.size() + 1) * sizeof(wchar_t);
		std::byte* const names = buffer.data() + header + names_header;
		std::memcpy(names, substitute.c_str(), substitute_bytes);
		std::memcpy(names + substitute_bytes, print.c_str(), (print.size() + 1) * sizeof(wchar_t));

		if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
		const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		bool set = false;
		if (handle != INVALID_HANDLE_VALUE) {
			DWORD returned = 0;
			set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
				nullptr, 0, &returned, nullptr) != FALSE;
			CloseHandle(handle);
		}
		if (!set) RemoveDirectoryW(link.c_str());
		return set;
	}

}
#endif
