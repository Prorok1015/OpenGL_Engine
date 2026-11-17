#pragma once
#include <snk/resource/res_diagnostics.h>
#include <snk/scene/level/scn_world_desc.h>
#include <span>
#include <string>
#include <vector>

namespace edt
{
	// A world of the open level as the Problems rows see it: the desc and the file its rows name (its own file, or the level's).
	struct instance_world
	{
		const scn::world_desc* world = nullptr;
		std::string file;
	};

	// What is wrong with the prefab instances of the open level (#150), as the diagnostics of one scope of the source "desc":
	// - a record that removes a node or component, or adds a node, on an instance: the editor never makes one, so it cannot change it;
	//   it stays in the file and is applied as written (a warning at the node);
	// - a prefab that, directly or through others it instantiates, instantiates itself: "Prefab 'X' refers to itself through 'Y'"
	//   (an error at the node of the level that starts the chain, once for each chain).
	// A row's `json_path` is "/children/<k>/..." in the world's file, so locate_desc leads to the node.
	std::vector<res::diagnostic> instance_diagnostics(std::span<const instance_world> worlds);
}
