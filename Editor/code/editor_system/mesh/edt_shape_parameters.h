#pragma once
#include "edt_desc_file_commands.h"
#include "edt_mesh_shapes.h"
#include <snk/scene/level/scn_prefab_desc.h>
#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace edt::mesh
{
	// "shapes/a.poly.desc" for "res://shapes/a.geom.desc" (the protocol is optional); empty when the tag is not a *.geom.desc.
	std::string poly_path_of_geometry(std::string_view geometry);

	// What the "Shape parameters" section of the Inspector knows of the shape a mesh node draws.
	struct shape_parameters
	{
		enum class state : std::uint8_t { editable, not_a_shape, lower_layer, edited_by_hand, unreadable };

		state what = state::not_a_shape;
		std::string poly;
		// code_version_of the poly file when it was read: shape.rebuild refuses a file that is no longer this one.
		std::string version;
		std::string kind;
		boost::json::object params;
		// Why the shape cannot be rebuilt (one line for the section); empty when it can.
		std::string reason;
	};

	// `file` is what read_desc_file gave for the poly file of `poly_path`; nothing is read here.
	shape_parameters load_shape_parameters(std::string poly_path, const desc_file_read& file);

	// How many mesh nodes below `root` (itself included) draw `geometry` with a range other than `range` (vx_begin, vx_end, ind_begin, ind_end;
	// a field a node does not have counts as 0): the nodes a rebuild changes. The protocol of a tag does not count.
	std::size_t count_stale_shape_nodes(const scn::prefab_desc::prefab_node& root, std::string_view geometry, const boost::json::object& range);
	// They take `range`; the number of nodes whose range was another one (the number count_stale_shape_nodes gave).
	std::size_t retarget_shape_nodes(scn::prefab_desc::prefab_node& root, std::string_view geometry, const boost::json::object& range);
	// The range the one mesh of a shape of `kind` with `params` has once it is built, as the fields above; nullopt, with `error`, when the parameters build nothing.
	std::optional<boost::json::object> predicted_range(std::string_view kind, const boost::json::object& params, std::string& error);
	// The range fields of an answer of shape.rebuild.
	std::optional<boost::json::object> range_of_rebuilt(const boost::json::object& answer);

	// What a person is told before Rebuild; every consequence is a line.
	struct rebuild_facts
	{
		std::string poly;
		bool level_open = false;
		// Mesh nodes of the open level / prefab whose range the rebuild changes (count_stale_shape_nodes).
		std::size_t level_nodes = 0;
		bool level_dirty = false;
		// Agent edits nobody accepted yet: the history boundary takes their marks away.
		bool level_agent_changes = false;
		bool prefab_open = false;
		std::size_t prefab_nodes = 0;
		bool prefab_dirty = false;
		// Why the open prefab is left as it is (Play, a file of a lower layer, a project that may not be written); empty: its nodes follow.
		std::string prefab_blocker;
	};
	std::string rebuild_confirmation(const rebuild_facts& facts);

	// shape.rebuild {path, version, kind, params}
	boost::json::object rebuild_arguments(const shape_parameters& shape, const boost::json::object& params);
}
