#pragma once
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace edt
{
	// The value of an environment variable as UTF-8; nullopt when it is not set.
	inline std::optional<std::string> environment_utf8(const char* name)
	{
#if defined(_WIN32)
		// std::getenv is C4996 under /WX; the wide value keeps a path with other letters intact.
		const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
		wchar_t* value = nullptr;
		std::size_t size = 0;
		if (_wdupenv_s(&value, &size, wide_name.c_str()) != 0 || !value) {
			return std::nullopt;
		}
		const std::u8string text = std::filesystem::path(value).u8string();
		std::free(value);
		return std::string(text.begin(), text.end());
#else
		const char* value = std::getenv(name);
		return value ? std::optional<std::string>(value) : std::nullopt;
#endif
	}
}
