#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <snk/platform/plt_beneath.h>
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
	// The FILE_ATTRIBUTE_* bits of `path` as the listing of its folder gives them (FindFirstFileExW on the exact name):
	// the entry itself is never opened, so neither a cloud file nor an entry of a projected file system (VFS for Git),
	// which asks its provider on open, is fetched. nullopt when it is not listed or cannot be read, and always off
	// Windows.
	std::optional<std::uint32_t> system_file_attributes(const std::filesystem::path& path);
	// `path` with each short (8.3) name of it spelled long (Windows: GetLongPathNameW, which lists the folder of each
	// component by its path, so it is asked only about a path agent_source_links_problem passed; a missing file keeps
	// its own name after its folder's long spelling); `path` itself elsewhere. An agent's read reaches a file by its
	// long names only: a short one is refused where the handle's final path is compared with the names.
	std::filesystem::path long_spelling(const std::filesystem::path& path);
	// platform::beneath::read_beneath with limits: the default of source_volume_ports::read_below.
	platform::beneath::read_result read_below_root(const platform::beneath::read_request& request,
		const platform::beneath::read_limits& limits, std::vector<std::byte>& out);

	// How an agent's read of a file reaches it: from `root` through `folder` to `leaf`, the device changing only at
	// the components of `folder` named in `mount_points` (read_limits::mount_points).
	struct agent_read_route {
		std::filesystem::path root;
		std::filesystem::path folder;
		std::filesystem::path leaf;
		std::vector<std::size_t> mount_points;
	};

	// The route of `path` (absolute, past agent_source_path_problem): with `windows` from its drive ("C:\"), no mount
	// point; else from "/", every mount point of `mount_table` on the way named, so no folder of the path is opened by
	// its path. nullopt when there is none.
	std::optional<agent_read_route> agent_read_route_of(const std::filesystem::path& path, bool windows,
		const std::optional<std::string>& mount_table);

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
		// agent_source_links_problem, Windows rules: the FILE_ATTRIBUTE_* bits of an entry, read before entry_of
		// opens it; null: not asked.
		std::function<std::optional<std::uint32_t>(const std::filesystem::path&)> file_attributes =
			system_file_attributes;
		// read_agent_file: the mount points its read may cross (agent_read_route_of). Not mount_table above: that one
		// judges what may be read, this one is where the disk really holds it.
		std::function<std::optional<std::string>()> read_mounts = system_mount_table;
		// agent_source_spelled: the long spelling of a path its walk passed (long_spelling).
		std::function<std::filesystem::path(const std::filesystem::path&)> long_name = long_spelling;
		// read_agent_file: the read below the volume's root (platform::beneath::read_beneath with read_limits).
		std::function<platform::beneath::read_result(const platform::beneath::read_request&,
			const platform::beneath::read_limits&, std::vector<std::byte>&)> read_below = read_below_root;
	};

	// The allowed source roots of an agent's import: only local fixed or removable volumes. Why `path` (as an agent
	// gave it, or as it resolved) may not be read for an agent; empty when it may. The text alone decides, plus the
	// drive table or the mount table: the path itself is never looked at. Refused: UNC and device paths (two leading
	// separators, `\??\`), a `.` or `..` component; Windows: no drive letter, a drive that is not
	// DRIVE_FIXED/DRIVE_REMOVABLE; POSIX: a path under /proc, or whose longest mount point (agent_log_mounts) has a
	// type outside the allow-list of local file systems (ext2/3/4, xfs, btrfs, zfs, f2fs, vfat, exfat, ntfs, ntfs3,
	// fuseblk, tmpfs, overlay). A lone `.` component is no component (`./tex.png`); `..` is refused.
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

	// The path an agent's read reaches `given` (absolute) by, or empty with `why`: agent_source_links_problem on
	// `given` as written first, so nothing on its way is followed or opened through, then its long spelling
	// (ports.long_name) and the walk again on that.
	std::filesystem::path agent_source_spelled(const std::filesystem::path& given, const source_volume_ports& ports,
		std::string& why);

	// The bytes of a file an agent's import reads, with the stamp of the handle they came from. `problem` is empty when
	// the read went through, else why (no path in it: an agent reads it).
	struct agent_file_read {
		std::vector<std::byte> bytes;
		std::optional<platform::beneath::file_stamp> stamp{};
		std::string problem;
		// With `problem`: the file or a folder on the way to it is not there.
		bool missing = false;
	};

	// Reads `path` (absolute, lexically normal) for an agent from the handle that was checked (#1412): every folder
	// from the root of its route down opened from the one above it without following a link, the device changing
	// only at a mount point, each entry judged by its folder's listing before it is opened, the file opened as itself
	// and refused when its attributes say a read would fetch it, at most `max_bytes`. Nothing is opened again by its
	// path afterwards. `stamp_only`: the checks and the stamp, no byte read.
	agent_file_read read_agent_file(const std::filesystem::path& path, std::uint64_t max_bytes,
		const source_volume_ports& ports, bool stamp_only = false);

	// Whether two stamps of an agent's read are of one file holding the same content: size, write time, file id and
	// device.
	bool same_file_stamp(const platform::beneath::file_stamp& a, const platform::beneath::file_stamp& b);

	// Why `path` is no longer the file an agent was shown with `shown`: a link on its way (agent_source_links_problem),
	// or the stamp of the handle a read opens (stamp only, nothing read) differs. Empty when it still is.
	std::string agent_file_changed(const std::filesystem::path& path,
		const std::optional<platform::beneath::file_stamp>& shown, const source_volume_ports& ports);

	// What stands in for the folder of the source in an error an agent reads.
	inline constexpr std::string_view k_source_folder_text = "<source folder>";

	// `text` with every spelling of each of `roots` read as res://, of `source` (when given) as its file name and of
	// the source's folder as k_source_folder_text (a side file a decoder names): what an error that reaches an agent
	// may hold of this machine's paths. `source` is the resolved path of a plan: only the roots are looked up on the
	// disk for their resolved spellings, the source and its folder never (#1408).
	std::string without_paths(std::string text, const std::vector<std::filesystem::path>& roots,
		const std::filesystem::path& source);
}
