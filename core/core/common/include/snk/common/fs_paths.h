#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace fs_paths
{
	// path::string() and generic_string() throw on MSVC for a name outside the active code page; a name seen by a
	// watcher or listed in the editor may be any Unicode. These give UTF-8 on every platform and never throw.
	inline std::string utf8_of(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(reinterpret_cast<const char*>(text.data()), text.size());
	}

	inline std::string generic_utf8_of(const std::filesystem::path& path)
	{
		const std::u8string text = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(text.data()), text.size());
	}

	inline std::filesystem::path path_from_utf8(std::string_view text)
	{
		return std::filesystem::path{ std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()) };
	}

	// Whole components: "/game/res2" is not inside "/game/res". Lexical: make both canonical first.
	inline bool is_inside(const std::filesystem::path& dir, const std::filesystem::path& candidate)
	{
		auto cand_it = candidate.begin();
		for (auto dir_it = dir.begin(); dir_it != dir.end(); ++dir_it, ++cand_it) {
			if (cand_it == candidate.end() || *dir_it != *cand_it) {
				return false;
			}
		}
		return true;
	}
}
