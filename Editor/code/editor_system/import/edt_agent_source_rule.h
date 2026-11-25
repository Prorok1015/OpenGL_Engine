#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <snk/platform/plt_link_check.h>

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
	// GetFileAttributesW of `path`: the entry itself (a link's own attributes), no data read, so nothing is fetched
	// from a cloud; nullopt when it cannot be read, and always off Windows.
	std::optional<std::uint32_t> system_file_attributes(const std::filesystem::path& path);

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
		// agent_source_links_problem: an entry opened as itself, never followed (platform::beneath::link_entry_at).
		std::function<std::optional<platform::beneath::link_entry>(const std::filesystem::path&, std::error_code&)>
			entry_of = platform::beneath::link_entry_at;
		// agent_source_links_problem, Windows rules: the FILE_ATTRIBUTE_* bits of an entry; null: not asked.
		std::function<std::optional<std::uint32_t>(const std::filesystem::path&)> file_attributes =
			system_file_attributes;
	};

	// The allowed source roots of an agent's import: only local fixed or removable volumes. Why `path` (as an agent
	// gave it, or as it resolved) may not be read for an agent; empty when it may. The text alone decides, plus the
	// drive table or the mount table: the path itself is never looked at. Refused: UNC and device paths (two leading
	// separators, `\??\`), a `.` or `..` component; Windows: no drive letter, a drive that is not
	// DRIVE_FIXED/DRIVE_REMOVABLE; POSIX: a path under /proc, or whose longest mount point (agent_log_mounts) has a
	// type outside the allow-list of local file systems (ext2/3/4, xfs, btrfs, zfs, f2fs, vfat, exfat, ntfs, ntfs3,
	// fuseblk, tmpfs, overlay).
	std::string agent_source_path_problem(std::string_view path, const source_volume_ports& ports);
	std::string agent_source_path_problem(std::string_view path);

	inline constexpr std::uint32_t k_file_attribute_offline = 0x1000u;	// FILE_ATTRIBUTE_OFFLINE
	inline constexpr std::uint32_t k_file_attribute_recall_on_open = 0x40000u;	// FILE_ATTRIBUTE_RECALL_ON_OPEN
	// FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
	inline constexpr std::uint32_t k_file_attribute_recall_on_data_access = 0x400000u;

	// Why an entry with these FILE_ATTRIBUTE_* bits may not be opened for an agent: opening or reading it would fetch it
	// from the network (an online-only OneDrive/Cloud Files placeholder, an offline file). Empty when it may: a cloud
	// file kept on this disk is a local file.
	std::string cloud_attributes_problem(std::uint32_t attributes);

	// Why `path` (absolute) may not be read for an agent, by what is on the disk: walks its prefixes from the root down,
	// each opened as itself (ports.entry_of), and refuses at the first that is no plain folder or file (a symlink, a
	// junction, a mount point, any other reparse point or one that cannot be inspected); on Windows also one whose
	// attributes say a read would fetch it from the network (cloud_attributes_problem). Nothing below a refused entry is
	// looked at; a missing one ends the walk (empty: the caller finds nothing there). `checked` (a folder that holds
	// `path` and passed this walk already): its own prefixes are not looked at again. Run it after
	// agent_source_path_problem, which keeps the walk off network volumes.
	std::string agent_source_links_problem(const std::filesystem::path& path, const source_volume_ports& ports,
		const std::filesystem::path& checked = {});

	// Why a side file at `candidate` (lexically normal, as the model names it) may not be read for an agent whose model
	// lies in `model_folder`: agent_source_path_problem, then agent_source_links_problem below the model's folder.
	std::string agent_side_file_problem(const std::filesystem::path& candidate,
		const std::filesystem::path& model_folder, const source_volume_ports& ports);

	// What stands in for the folder of the source in an error an agent reads.
	inline constexpr std::string_view k_source_folder_text = "<source folder>";

	// `text` with every spelling of each of `roots` read as res://, of `source` (when given) as its file name and of
	// the source's folder as k_source_folder_text (a side file a decoder names): what an error that reaches an agent
	// may hold of this machine's paths. `source` is the resolved path of a plan: only the roots are looked up on the
	// disk for their resolved spellings, the source and its folder never (#1408).
	std::string without_paths(std::string text, const std::vector<std::filesystem::path>& roots,
		const std::filesystem::path& source);
}
