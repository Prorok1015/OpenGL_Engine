#pragma once
#include <snk/resource/res_diagnostics.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <span>
#include <string>
#include <vector>

namespace edt
{
	// A world of the open level as the rows see it: the desc, the file its rows name and the pointer of the world inside that file
	// ("" for a world with a file of its own, "/worlds/<i>" for one written inline in the level's file).
	struct instance_world
	{
		const scn::world_desc* world = nullptr;
		std::string file;
		std::string pointer_prefix;
	};

	// What is wrong with the prefab instances of the open level (#150), one row each: a `remove` or `add_node` record on an instance
	// (a warning at the node; the editor never makes one), and a prefab the node instantiates that instantiates itself, said from the
	// node's own prefab: "Prefab 'X' refers to itself" or "... through 'Y' (X -> Y -> X)". With `assembler`, also a `set` record that
	// the steps of its component's type remove (a warning) or put in more than one place (an error), as scn::find_stale_records
	// judges it by expanding the node (#1255). A row's `json_path` is the node's pointer in `file`.
	std::vector<res::diagnostic> instance_diagnostics(std::span<const instance_world> worlds, const scn::ecs_assembler* assembler = nullptr);
}
