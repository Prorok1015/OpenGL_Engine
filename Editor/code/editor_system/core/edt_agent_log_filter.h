#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What `log.read` gives a caller that is not the person at the editor (#935). The journal holds every category: a path
// on disk a command took care not to return, the output of a build while an agent may not build, the address of the
// agent server. An agent gets the journal without the categories below and with paths outside the project made
// anonymous; a person reads it whole.
namespace edt
{
	// The folders a path in a log line may be under, and what stands in for each. Empty: none.
	struct agent_log_roots
	{
		std::filesystem::path project;
		std::filesystem::path home;
		std::filesystem::path temp;
		// The names directly under the root of the file system ("home", "usr", "tmp"): what tells an absolute path from
		// a JSON pointer such as "/header/type", which looks the same. Not used on Windows, where a path names its drive.
		std::vector<std::string> top_level;
	};

	// The roots of this process: the project folder given, the user's home, the temp folder, the entries of "/".
	agent_log_roots agent_log_roots_of(const std::filesystem::path& project);

	// Whether a caller that is not a person gets entries of `category` (or of one below it: "editor/build/x").
	// RULE: a new log category whose lines can hold a secret, a credential, the content of a file outside the project or
	// the output of something an agent may not run goes into the list in the .cpp, in the same change that adds it.
	bool agent_log_category_visible(std::string_view category, bool build_allowed);

	// `text` with the project folder written as "<project>", the home folder as "<home>", the temp folder as "<tmp>"
	// (what follows stays: "<project>/res/a.desc") and any other absolute path as "<path>".
	std::string agent_log_text(std::string_view text, const agent_log_roots& roots);
}
