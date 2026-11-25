#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where an agent's import may read its source from (#1378, #1380, docs/epics/editor/EPIC-62.md): one rule for the
// picture and the model import. Told from the text of the path and the volume tables only, never from the path itself:
// a look at `\\server\share` connects to the server (sending the user's credentials on Windows) and a dead network
// mount hangs the caller.
namespace edt
{
	// DRIVE_REMOVABLE and DRIVE_FIXED of GetDriveTypeW: the only drive types an agent's source may lie on.
	inline constexpr unsigned k_drive_removable = 2;
	inline constexpr unsigned k_drive_fixed = 3;

	// GetDriveTypeW of "X:\" (the local drive table only, no network round trip); 0 (DRIVE_UNKNOWN) off Windows.
	unsigned system_drive_type(char letter);
	// The text of /proc/self/mounts; nullopt when it cannot be read (and on Windows).
	std::optional<std::string> system_mount_table();

	// How agent_source_path_problem tells a local volume from another one, without touching the path.
	struct source_volume_ports
	{
#if defined(_WIN32)
		bool windows = true;
#else
		bool windows = false;
#endif
		// Windows rules: the type of a drive by its letter; null: unknown.
		std::function<unsigned(char letter)> drive_type = system_drive_type;
		// POSIX rules: the mount table; null or nullopt: it cannot be read, every path is refused.
		std::function<std::optional<std::string>()> mount_table = system_mount_table;
	};

	// The allowed source roots of an agent's import: only local fixed or removable volumes. Why `path` (as an agent
	// gave it, or as it resolved) may not be read for an agent; empty when it may. The text alone decides, plus the
	// drive table or the mount table: the path itself is never looked at. Refused: UNC and device paths (two leading
	// separators, `\??\`), a `.` or `..` component; Windows: no drive letter, a drive that is not
	// DRIVE_FIXED/DRIVE_REMOVABLE; POSIX: a path whose longest mount point is a network or automount one
	// (agent_log_mounts).
	std::string agent_source_path_problem(std::string_view path, const source_volume_ports& ports);
	std::string agent_source_path_problem(std::string_view path);

	// `text` with every spelling of each of `roots` read as res:// and of `source` (when given) as its file name: what
	// an error that reaches an agent may hold of this machine's paths.
	std::string without_paths(std::string text, const std::vector<std::filesystem::path>& roots,
		const std::filesystem::path& source);
}
