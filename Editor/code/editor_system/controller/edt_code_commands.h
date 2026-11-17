#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace res
{
	class resource_system;
}

namespace edt
{
	class editor_events;

	// code.list/read/write/create/delete/rename (A7, #451): an agent edits the project's code/ folder through code://
	// (#450), never through file system paths. Paths are '/' separated and relative to code/; every one passes
	// check_code_file_path (or the folder check of code.list) before anything is touched, and the resolver's safe path
	// checks again. code.write and code.delete take the `version` code.read gave: another one means someone changed the
	// file since, and nothing is written. Text is UTF-8 and kept byte for byte (CRLF stays CRLF). Main thread only.
	struct code_commands_deps
	{
		// The resource system that mounts code:// (the editor's base one).
		res::resource_system& res;
		// agent_action after each change an agent made; null: none.
		editor_events* events = nullptr;
	};

	[[nodiscard]] std::vector<registration> register_code_commands(cmd::command_registry& commands, code_commands_deps deps);

	// What code.read and code.write may move in one call.
	inline constexpr std::size_t k_code_file_limit = 1024 * 1024;

	// "<fnv1a-64 hex>-<size>" of a file's bytes: what code.read gives and code.write/code.delete expect back.
	std::string code_version_of(std::span<const std::byte> bytes);

	// The policy's category of a call on `relative`: cmake_presets for a CMake file (CMakeLists.txt, *.cmake,
	// CMake*Presets.json, whatever the case), else `otherwise`. A path the commands refuse anyway gets `otherwise`.
	cmd::agent_category code_call_category(std::string_view relative, cmd::agent_category otherwise);

	// Strict UTF-8: no overlong forms, surrogates or code points past U+10FFFF.
	bool is_valid_utf8(std::string_view text);
}
