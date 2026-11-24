#pragma once
#include "edt_agent_job_result.h"
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What `log.read` gives a caller that is not the person at the editor (#935). The journal holds every category: a path
// on disk a command took care not to return, the output of a build while an agent may not build, the address of the
// agent server. An agent gets the journal without the categories below and with paths outside the project made
// anonymous; a person reads it whole.
namespace edt
{
	class agent_policy;

	// The top two levels of the disk as they were when it was listed: what tells an absolute path under none of the
	// roots from a JSON pointer such as "/header/type", which looks the same. The text of a line is only ever compared
	// with it: no file system call is made for a line (that would tell an agent what exists, and may block).
	class agent_log_disk
	{
	public:
		agent_log_disk() = default;
		// Puts `names` in the order the lookup needs (the only way to fill one, so it cannot be forgotten). A name
		// that has no two segments is dropped; one whose first segment holds '\' goes to `unlisted` by that segment.
		agent_log_disk(std::vector<std::string> names, std::vector<std::string> unlisted, bool whole_root = false);

		// Names of two segments with '/': "/usr/bin", "/media/John Smith", "C:/Program Files". A token that starts
		// with one (as whole segments; a space in it counts) is a path. Sorted, so that a token is looked up by its
		// first segment with a binary search.
		const std::vector<std::string>& names() const { return m_names; }
		// First segments whose content is not known ("/proc", a network mount, a folder too large or not readable,
		// one met after the total was used up): anything below one is a path, except right after a resource name
		// (there it may as well be a JSON pointer).
		const std::vector<std::string>& unlisted() const { return m_unlisted; }
		// The root "/" itself is not known (more unread folders than agent_log_disk_limits allows; on Windows a
		// current drive that is not listed): every rooted token ("/a", "/a/b") is a path, with the same exception.
		bool whole_root() const { return m_whole_root; }

	private:
		std::vector<std::string> m_names;
		std::vector<std::string> m_unlisted;
		bool m_whole_root = false;
	};

	struct agent_log_disk_limits
	{
		// Entries of `names` and `unlisted` together while folders are still entered; after that no folder is
		// entered and every first-level name left goes to `unlisted`.
		std::size_t total = 4096;
		// A folder with more entries goes to `unlisted` as a whole, so /usr/bin cannot use up the total. Also the
		// most names one root gives to `unlisted`: a root with more counts as a path whole (`whole_root`, or the
		// drive itself in `unlisted`), which fails closed.
		std::size_t per_folder = 512;
	};

	// What `root` holds two levels down, as names from `root` ("/a/b"). The folders of `skipped` (first-level names:
	// "proc") are not entered and go to `unlisted`. An error ends that folder; nothing is thrown.
	agent_log_disk agent_log_disk_of(const std::filesystem::path& root, std::span<const std::string> skipped, agent_log_disk_limits limits = {});

	// The disk of this machine, listed now (once, when the editor starts). Not entered: /net, /proc, /sys, /dev and
	// the first-level network and automount points of /proc/self/mounts. On Windows: every drive the system has, by
	// agent_log_drive_is_listed ("C:/Users/x", or "Z:" in `unlisted` without a file system call), and the current
	// drive ("/Users/x") when it is a listed one; when it is not, `whole_root` is set and it is not read.
	agent_log_disk agent_log_disk_at_start();

	// DRIVE_FIXED of GetDriveTypeW.
	inline constexpr unsigned k_agent_log_drive_fixed = 3;

	// Whether the drive `letter` of the type GetDriveTypeW gave is listed two levels down. Only a fixed disk from C:
	// on is; a network drive, one without a root, a removable one, a CD, a RAM disk and an unknown type are not read
	// at all (a drive that is gone can block for seconds) and count as a path whole.
	bool agent_log_drive_is_listed(char letter, unsigned type);

	// The first-level folders (names without '/': "mnt share") that `mounts`, the text of /proc/self/mounts, says are
	// network or automount points (nfs*, cifs, smb3, autofs, 9p, ceph, glusterfs, davfs, fuse and every fuse.*, which
	// cannot be told from a network one by its name). Octal escapes of a mount point are decoded
	// ("\040" is a space); a mount point further down than the first level is left out.
	std::vector<std::string> agent_log_remote_mounts(std::string_view mounts);

	// One line of /proc/self/mounts: the mount point (absolute, octal escapes decoded) and whether its type is one
	// agent_log_remote_mounts counts as a network or automount one.
	struct agent_log_mount
	{
		std::string point;
		bool remote = false;
	};
	// Every mount point of `mounts`, at any depth, in the order of the text.
	std::vector<agent_log_mount> agent_log_mounts(std::string_view mounts);

	// The folders a path in a log line may be under, and what stands in for each. Empty: none. A folder may be listed
	// in every spelling a line can have (as given and as the system resolves it); a trailing separator does not count.
	struct agent_log_roots
	{
		std::vector<std::filesystem::path> project;
		std::vector<std::filesystem::path> home;
		std::vector<std::filesystem::path> temp;
		// Empty: no rooted token ("/a/b") outside the roots is a path. Asked on Windows too, where "/Users/dev" is a
		// path on the current drive and "/c/Users" one on drive C: (found by the name "C:/Users").
		agent_log_disk disk;
	};

	// The roots of this process: the project folder given, the user's home and the temp folder, each absolute and
	// normalized and, where that differs, as the file system resolves it (a linked folder, /private/tmp on macOS),
	// and agent_log_disk_at_start. Call it once per process: it lists the disk.
	agent_log_roots agent_log_roots_of(const std::filesystem::path& project);

	// Whether a caller that is not a person gets entries of `category` (or of one below it: "editor/build/x").
	// RULE: a new log category whose lines can hold a secret, a credential, the content of a file outside the project or
	// the output of something an agent may not run goes into the list in the .cpp, in the same change that adds it.
	bool agent_log_category_visible(std::string_view category, bool build_allowed);

	// `text` with the project folder written as "<project>", the home folder as "<home>", the temp folder as "<tmp>"
	// (what follows stays: "<project>/res/a.desc") and any other absolute path as "<path>". A path is looked for after
	// any character a path cannot hold, after a compiler flag ("-I/usr/x", "/FoC:\x") and in a file:// address; a
	// folder name matches a root only whole ("/p/demo" is not the root of "/p/demo2"). A path outside the roots ends
	// at a space unless `roots.disk` has the folder with the space, and at a ':' before '/' (a PATH list); a trailing
	// ":<line>" or ":<line>:<col>" stays after the stand-in ("<path>:10:5"). Reads `text` and `roots` only.
	std::string agent_log_text(std::string_view text, const agent_log_roots& roots);

	// Whether the policy lets an agent see what a build prints: only while it may build without being asked.
	bool agent_build_output_allowed(agent_policy& policy, const cmd::command_registry& commands);

	// Fills the engine's filter for the editor: a person (origin ui) reads everything as it was logged; anyone else gets
	// the categories of agent_log_category_visible and every text through agent_log_text. `build_allowed` is asked a
	// few times a second at most. Entries of `k_agent_build_category` (edt_agent_job_result.h) stand for build output
	// in other commands too.
	void install_agent_log_filter(cmd::log_read_filter& filter, std::function<bool()> build_allowed, agent_log_roots roots);
}
