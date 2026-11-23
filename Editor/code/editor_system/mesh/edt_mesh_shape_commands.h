#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include "edt_desc_file_commands.h"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_file_change.h>
#include <functional>
#include <optional>
#include <string>
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
}

namespace edt::mesh
{
	// What shape.create and shape.rebuild (#746) work on. All of it outlives the registrations.
	struct shape_command_deps
	{
		shape_command_deps(res::resource_system& resources, const desc::desc_system& descriptions, desc_file_layout where,
			cmd::command_registry& registry, editor_events* sink = nullptr)
			: res(resources), descs(descriptions), layout(std::move(where)), commands(registry), events(sink) {}

		res::resource_system& res;
		const desc::desc_system& descs;
		// Where the project's res:// folders are: the same functions as the desc.* commands have.
		desc_file_layout layout;
		// The registry the scene commands are in: shape.add makes its node with scene.node.create, shape.rebuild moves the nodes' ranges
		// with shape.update_nodes, so both are one scene edit under the caller's origin.
		cmd::command_registry& commands;
		// agent_action after each file an agent wrote; null: none.
		editor_events* events;
		// How the files are written; null: rnd::write_geometry_files and resource_system::store_sync. The tests put a failing one here.
		std::function<std::string(const res::tag&, const rnd::geometry_desc&)> write_geometry;
		std::function<bool(const res::tag&, const std::vector<std::byte>&)> write_poly;
	};

	// shape.create: a parametric shape as two files of the project, <folder>/<name>.poly.desc (the editable source, editor only,
	// nothing refers to it at run time) and <folder>/<name>.geom.desc (what a mesh node refers to). Never replaces a file.
	// shape.add: a node with the mesh of such a shape (poly: the source file), sized by `scale`.
	// shape.rebuild: new parameters (or the stored ones) into both files of an existing shape; a person confirms each time, and
	// a mesh that was edited by hand is refused (check_shape_source). The parameters are checked by make_shape before a byte is written.
	[[nodiscard]] std::vector<registration> register_shape_commands(cmd::command_registry& commands, shape_command_deps deps);
}
