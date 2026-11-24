#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <string>

namespace test_util {

	// The empty folder or file `entry` made a reparse point with a name-surrogate tag of a third party (#1295, #1343):
	// std::filesystem calls it neither a symlink nor a junction. Needs no privilege. Empty on success, else why not.
	inline std::string set_third_party_surrogate_tag(const std::filesystem::path& entry)
	{
		const HANDLE handle = CreateFileW(entry.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			return "cannot open the entry: error " + std::to_string(GetLastError());
		}
		REPARSE_GUID_DATA_BUFFER buffer{};
		buffer.ReparseTag = 0x2000ABCDu;
		buffer.ReparseDataLength = 0;
		buffer.ReparseGuid = { 0x1295ABCDu, 0x1295, 0x4295, { 0x12, 0x95, 0x12, 0x95, 0x12, 0x95, 0x12, 0x95 } };
		DWORD returned = 0;
		const BOOL set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE,
			nullptr, 0, &returned, nullptr);
		const DWORD code = GetLastError();
		CloseHandle(handle);
		return set ? std::string{} : "FSCTL_SET_REPARSE_POINT failed: error " + std::to_string(code);
	}

}
#endif
