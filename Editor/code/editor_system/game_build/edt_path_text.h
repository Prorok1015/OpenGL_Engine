#pragma once
#include <filesystem>
#include <string>

namespace edt
{
	// UTF-8 text of a path; path::string() throws on Windows for a name outside the ANSI code page.
	inline std::string utf8_of(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}
}
