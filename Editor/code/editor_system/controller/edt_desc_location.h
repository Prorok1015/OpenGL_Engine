#pragma once
#include <snk/scene/level/scn_prefab_desc.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scn { class level_desc; }

namespace edt
{
	// Where a row of Problems leads (#596): a node of a world of a level, a whole world, or a file to open.
	struct desc_target
	{
		// The level's file the world belongs to; empty when no world is named.
		std::string level;
		std::string world;
		// Node ids from the children of the world's root; empty: the world itself.
		std::vector<std::uint64_t> path;
		// A file to open; set only when no world is named.
		std::string resource;

		bool empty() const { return world.empty() && resource.empty(); }
	};

	// A level as one load of its file read it. The pointers point into that load: use it while its handle lives.
	struct level_worlds
	{
		struct world
		{
			std::string name;
			const scn::prefab_desc::prefab_node* root = nullptr;
			// The tag its diagnostics name: its own file, or the copy of an inline world.
			std::string file;
		};

		// What a target names as its level.
		std::string level;
		// The tags the level's own diagnostics carry (its working copy, its file).
		std::vector<std::string> names;
		std::vector<world> worlds;
	};

	// The worlds of a loaded level, in file order; a world that did not load has no name and leads nowhere.
	level_worlds level_worlds_of(const scn::level_desc& desc, std::string level, std::vector<std::string> names);

	// A diagnostic of `resource` at `json_pointer`: "/worlds/<i>/children/<k>/..." of the level, or "/children/<k>/..." of
	// a world's file, leads to the deepest node the path names (it stops at anything but a child); any other resource is
	// a file to open.
	desc_target locate_desc(const level_worlds& level, std::string_view resource, std::string_view json_pointer);
}
