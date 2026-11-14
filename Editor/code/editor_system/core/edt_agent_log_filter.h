#pragma once
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <filesystem>
#include <functional>
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

	// The folders a path in a log line may be under, and what stands in for each. Empty: none. A folder may be listed
	// in every spelling a line can have (as given and as the system resolves it); a trailing separator does not count.
	struct agent_log_roots
	{
		std::vector<std::filesystem::path> project;
		std::vector<std::filesystem::path> home;
		std::vector<std::filesystem::path> temp;
		// Whether a folder is on the disk: what tells an absolute path under none of the roots from a JSON pointer such
		// as "/header/type", which looks the same; a token is a path only when its first two segments exist. Asked on
		// Windows too, where "/Users/dev" is a path on the current drive. Empty: the file system is asked.
		std::function<bool(const std::filesystem::path&)> on_disk;
	};

	// The roots of this process: the project folder given, the user's home and the temp folder, each absolute and
	// normalized and, where that differs, as the file system resolves it (a linked folder, /private/tmp on macOS).
	agent_log_roots agent_log_roots_of(const std::filesystem::path& project);

	// Whether a caller that is not a person gets entries of `category` (or of one below it: "editor/build/x").
	// RULE: a new log category whose lines can hold a secret, a credential, the content of a file outside the project or
	// the output of something an agent may not run goes into the list in the .cpp, in the same change that adds it.
	bool agent_log_category_visible(std::string_view category, bool build_allowed);

	// `text` with the project folder written as "<project>", the home folder as "<home>", the temp folder as "<tmp>"
	// (what follows stays: "<project>/res/a.desc") and any other absolute path as "<path>". A path is looked for after
	// any character a path cannot hold, after a compiler flag ("-I/usr/x", "/FoC:\x") and in a file:// address; a
	// folder name matches a root only whole ("/p/demo" is not the root of "/p/demo2").
	std::string agent_log_text(std::string_view text, const agent_log_roots& roots);

	// Whether the policy lets an agent see what a build prints: only while it may build without being asked.
	bool agent_build_output_allowed(agent_policy& policy, const cmd::command_registry& commands);

	// Fills the engine's filter for the editor: a person (origin ui) reads everything as it was logged; anyone else gets
	// the categories of agent_log_category_visible and every text through agent_log_text. `build_allowed` is asked a
	// few times a second at most. Entries of `k_agent_build_category` stand for build output in other commands too.
	void install_agent_log_filter(cmd::log_read_filter& filter, std::function<bool()> build_allowed, agent_log_roots roots);

	inline constexpr std::string_view k_agent_build_category = "editor/build";
}
