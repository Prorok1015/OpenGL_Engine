#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "test_file_attributes.hpp"
#include <filesystem>
#include <string>
#include <windows.h>

namespace test_util {

	bool set_offline(const std::filesystem::path& path, bool offline)
	{
		const DWORD attributes = ::GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;
		const DWORD offline_bit = FILE_ATTRIBUTE_OFFLINE;
		const DWORD wanted = offline ? (attributes | offline_bit) : (attributes & ~offline_bit);
		return ::SetFileAttributesW(path.c_str(), wanted == 0 ? FILE_ATTRIBUTE_NORMAL : wanted) != FALSE;
	}

	std::filesystem::path short_path(const std::filesystem::path& path)
	{
		const DWORD length = ::GetShortPathNameW(path.c_str(), nullptr, 0);
		if (length == 0) return {};
		std::wstring out(length, L'\0');
		const DWORD written = ::GetShortPathNameW(path.c_str(), out.data(), length);
		if (written == 0 || written >= length) return {};
		out.resize(written);
		return std::filesystem::path(out);
	}

}
#endif
