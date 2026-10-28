#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <snk/common/snk_api.h>

namespace platform
{
	// The file of the running program; empty when the system cannot tell.
	SNK_API std::filesystem::path current_executable();

	// Starts `program` with `args` (the program itself not among them, UTF-8) and does not wait for it: the new process
	// outlives this one. nullopt when it started, otherwise why it did not.
	SNK_API std::optional<std::string> spawn_detached(const std::filesystem::path& program, const std::vector<std::string>& args);
}
