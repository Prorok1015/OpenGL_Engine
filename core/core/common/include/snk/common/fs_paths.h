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

	// As is_inside, each part compared with ASCII letters folded: the same folder on a case-insensitive file system.
	inline bool is_inside_any_case(const std::filesystem::path& dir, const std::filesystem::path& candidate)
	{
		const auto folded = [](const std::filesystem::path& part) {
			std::string text = generic_utf8_of(part);
			for (char& ch : text) {
				if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
			}
			return text;
		};
		auto cand_it = candidate.begin();
		for (auto dir_it = dir.begin(); dir_it != dir.end(); ++dir_it, ++cand_it) {
			if (cand_it == candidate.end() || folded(*dir_it) != folded(*cand_it)) {
				return false;
			}
		}
		return true;
	}
}
