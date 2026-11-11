#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>
#include <utility>

namespace edt::vcs
{
	struct file_stamp
	{
		// Write time in the clock's own ticks: only compared.
		std::int64_t mtime = 0;
		std::uint64_t size = 0;

		bool operator==(const file_stamp&) const = default;
	};

	// nullopt: no such file or folder (or it cannot be read); a folder's size is 0. Never throws. A link is followed,
	// as git follows it: a dangling `.git` link is no marker. The watcher and vcs_service both use it (#561).
	inline std::optional<file_stamp> stat_entry(const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::filesystem::file_status status = std::filesystem::status(path, ec);
		if (ec || !std::filesystem::exists(status)) return std::nullopt;
		const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, ec);
		if (ec) return std::nullopt;
		file_stamp out;
		out.mtime = static_cast<std::int64_t>(time.time_since_epoch().count());
		if (std::filesystem::is_regular_file(status)) {
			const std::uintmax_t size = std::filesystem::file_size(path, ec);
			if (!ec) out.size = static_cast<std::uint64_t>(size);
		}
		return out;
	}

	struct repository_marker_state
	{
		std::optional<std::filesystem::path> path;
		// Compared only while the repository is unavailable: a probe that caught `.git` half-written (a `git init`
		// still running, a worktree's `gitdir:` fixed later) is probed again when it is finished.
		std::optional<file_stamp> stamp;

		bool operator==(const repository_marker_state&) const = default;
	};

	using stat_function = std::function<std::optional<file_stamp>(const std::filesystem::path&)>;

	// `\\server\share`: the share is the top, `\\server\.git` is nothing anyone can make (`\\?\` and `\\.\` are local).
	inline bool is_network_root_name(const std::filesystem::path& root_name)
	{
		const auto& text = root_name.native();
		const auto separator = [](auto c) { return c == '/' || c == '\\'; };
		return text.size() > 2 && separator(text[0]) && separator(text[1]) && text[2] != '?' && text[2] != '.';
	}

	// Where git finds the repository of `project` (#561): the first `.git` entry, a folder or the file of a linked
	// worktree or submodule (`gitdir: ...`), in the project folder or one of its parents. Only `stat` is asked, git
	// never runs; a change of the answer is what makes vcs_service probe the repository again.
	inline repository_marker_state repository_marker(const std::filesystem::path& project, const stat_function& stat)
	{
		std::filesystem::path dir = project;
		if (!dir.has_root_directory()) {
			// A relative root would stop at "" instead of the file system's root.
			std::error_code ec;
			std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
			if (!ec) dir = std::move(absolute);
		}
		dir = dir.lexically_normal();
		if (!dir.has_filename() && dir.has_relative_path()) dir = dir.parent_path();
		const bool network_share = is_network_root_name(dir.root_name());
		for (;;) {
			std::filesystem::path marker = dir / ".git";
			if (std::optional<file_stamp> stamp = stat(marker)) return { std::move(marker), *stamp };
			if (!dir.has_relative_path()) break;
			std::filesystem::path parent = dir.parent_path();
			if (parent == dir || (network_share && !parent.has_relative_path())) break;
			dir = std::move(parent);
		}
		return {};
	}
}
