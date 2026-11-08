#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <snk/resource/res_diagnostics.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace desc
{
	class desc_system;
}

namespace res
{
	class resource_system;
}

namespace edt
{
	class editor_events;

	// desc.read / desc.create / desc.write (#550): an agent reads and writes the desc files (*.desc: levels, materials,
	// prefabs...) of the project's res/ folder. Every path passes check_agent_level_path (inside the project, no link,
	// no drive or protocol, *.desc only) before the disk is touched; the content is a JSON object that must fit the
	// schemas of its `__type` objects before a byte is written. desc.write replaces an existing file and asks a person
	// each time; desc.create never replaces. Main thread only.
	struct desc_file_deps
	{
		desc_file_deps(res::resource_system& resources, const desc::desc_system& descriptions) : res(resources), descs(descriptions) {}

		res::resource_system& res;
		const desc::desc_system& descs;
		// The folder res:// writes into (the top layer of the project that is open now).
		std::function<std::filesystem::path()> top_layer;
		// Every folder res:// is read from, the top one last. Null: only top_layer.
		std::function<std::vector<std::filesystem::path>()> layers;
		// Project-relative paths of the files the open level lives in (levels/a.desc and its editor copy): they belong
		// to the document in memory, which would not follow a write. Null: none.
		std::function<std::vector<std::string>()> open_level_files;
		// agent_action after each change an agent made; null: none.
		editor_events* events = nullptr;
	};

	[[nodiscard]] std::vector<registration> register_desc_file_commands(cmd::command_registry& commands, desc_file_deps deps);

	// What desc.read gives and desc.write takes back is edt::code_version_of(bytes); one call moves at most this many bytes.
	inline constexpr std::size_t k_desc_file_limit = 1024 * 1024;

	// Every problem of the desc objects under `root` (the file's content): a root that is no object with a `__type`, an
	// unknown `__type`, a `__version` other than the current one, a field the type does not have or a value that does not
	// fit it, found in each object with a `__type` at any depth (a field holding an inline desc, an array of them).
	// One entry per object at most, `json_path` the JSON pointer of the field in `resource`. Nothing is logged.
	std::vector<res::diagnostic> validate_desc_tree(const desc::desc_system& descs, const boost::json::value& root, std::string_view resource);
}
