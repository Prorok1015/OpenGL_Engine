#pragma once
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/value.hpp>
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
	struct agent_log_disk
	{
		// Names of two segments with '/': "/usr/bin", "/media/John Smith", "C:/Program Files". A token that starts
		// with one (as whole segments; a space in it counts) is a path.
		std::vector<std::string> names;
		// First segments whose content is not known ("/proc", a network mount, a folder too large or not readable):
		// anything below one is a path, except right after a resource name (there it may as well be a JSON pointer).
		std::vector<std::string> unlisted;
	};

	struct agent_log_disk_limits
	{
		// Entries of `names` and `unlisted` together; the listing stops silently there.
		std::size_t total = 4096;
		// A folder with more entries goes to `unlisted` as a whole, so /usr/bin cannot use up the total.
		std::size_t per_folder = 512;
	};

	// What `root` holds two levels down, as names from `root` ("/a/b"). The folders of `skipped` (first-level names:
	// "proc") are not entered and go to `unlisted`. An error ends that folder; nothing is thrown.
	agent_log_disk agent_log_disk_of(const std::filesystem::path& root, std::span<const std::string> skipped, agent_log_disk_limits limits = {});

	// The disk of this machine, listed now (once, when the editor starts). Not entered: /net, /proc, /sys, /dev and
	// the first-level network and automount points of /proc/self/mounts. On Windows: the current drive ("/Users/x")
	// and every drive C: to Z: whose root exists ("C:/Users/x").
	agent_log_disk agent_log_disk_at_start();

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
	// few times a second at most. Entries of `k_agent_build_category` stand for build output in other commands too.
	void install_agent_log_filter(cmd::log_read_filter& filter, std::function<bool()> build_allowed, agent_log_roots roots);

	inline constexpr std::string_view k_agent_build_category = "editor/build";

	// The message fields of job results an agent sees redacted (vcs, forge, migration); `text` is not one: a diff.
	inline constexpr std::array<std::string_view, 7> k_agent_job_result_fields{
		"stderr_tail", "message", "error", "output", "refused", "reason", "notes",
	};
	// `text` is redacted only in an object that also has this key: every migration answer has it, a diff never does.
	inline constexpr std::string_view k_agent_job_result_text_marker = "code";
	// How many objects deep (the result itself is the first) those fields are looked for: report.files[].warnings[].
	inline constexpr int k_agent_job_result_depth = 4;

	// `text` as `from` may read it: through the filter's `redact`; as it is without a filter or without `redact`.
	std::string agent_shown_text(const cmd::log_read_filter* filter, std::string_view text, const cmd::origin& from);

	// A job's result as `from` may read it: every string (or string of an array) under a name of
	// k_agent_job_result_fields, in objects and arrays of objects down to k_agent_job_result_depth, goes through
	// agent_shown_text, and so does a string `text` beside a k_agent_job_result_text_marker key; every other value
	// (a diff, a path, a subject) stays as the command made it.
	boost::json::value agent_job_result(const boost::json::value& result, const cmd::log_read_filter* filter, const cmd::origin& from);
}
