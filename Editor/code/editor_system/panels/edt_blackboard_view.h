#pragma once
#include <snk/ecs/ecs_channel.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <string>
#include <string_view>
#include <vector>

// ImGui-free part of the Blackboard panel (WORLD_MESSAGING 10: "the viewer in the editor" keeps the blackboard from
// turning into a dump of globals): what the committed blackboard of a set looks like as table rows.
namespace edt::blackboard_view
{
	struct row
	{
		std::string key;
		// The part of the key before the first dot ("hud" for "hud.health"): the key prefix names the owner.
		std::string owner;
		std::string type;
		std::string value;
		// "external" for the editor or the shell, else the name of the context that wrote the value last
		// (its id when that context is gone).
		std::string writer;
	};

	// "hud" for "hud.health"; empty for a key without a dot.
	std::string owner_of(std::string_view key);

	// The type of the entry by the name the editor shows ("int", "vec3", ...); the type name of the entry when
	// the editor does not know it.
	std::string type_label(const ecs::bb_entry& entry);

	// The value as text. A type the editor does not know shows as "<type name>".
	std::string format_value(const ecs::bb_entry& entry);

	// Rows of the committed blackboard of the set (what the systems read this frame), sorted by key like the
	// blackboard itself. `filter` keeps the keys that contain it, ignoring case; empty keeps all.
	std::vector<row> collect_rows(const scn::context_set& set, std::string_view filter = {});

	// Names of the sets in creation order.
	std::vector<std::string> set_names(scn::context_manager& manager);

	// Keeps `current` while the set exists; else "level", else the first one; empty when there are no sets.
	std::string pick_set(std::string_view current, const std::vector<std::string>& names);
}
