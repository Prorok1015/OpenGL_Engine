#pragma once
#include <filesystem>

namespace fs_paths
{
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
