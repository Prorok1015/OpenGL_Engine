#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>

namespace edt::vcs
{
	// Where git finds the repository of `project` (#561): the first `.git` entry, a folder or the file of a linked
	// worktree or submodule (`gitdir: ...`), in the project folder or one of its parents. Only `exists` is asked, git
	// never runs; a change of the answer is what makes vcs_service probe the repository again.
	inline std::optional<std::filesystem::path> repository_marker(const std::filesystem::path& project,
		const std::function<bool(const std::filesystem::path&)>& exists)
	{
		for (std::filesystem::path dir = project; !dir.empty();) {
			std::filesystem::path marker = dir / ".git";
			if (exists(marker)) return marker;
			std::filesystem::path parent = dir.parent_path();
			if (parent == dir) break;
			dir = std::move(parent);
		}
		return std::nullopt;
	}

	inline bool entry_exists(const std::filesystem::path& path)
	{
		std::error_code ec;
		return std::filesystem::exists(std::filesystem::symlink_status(path, ec));
	}
}
