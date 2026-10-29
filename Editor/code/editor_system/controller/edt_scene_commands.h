#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <functional>
#include <vector>

namespace edt
{
	class document_manager;
	class selection_service;
	struct editor_services;

	// What the commands that change the scene (A5, #291) work on. All of it outlives the registrations.
	struct scene_command_sources
	{
		document_manager& documents;
		selection_service& selection;
		const editor_services& services;
		// A person's gesture is in progress (a drag, an edit waiting in a field): an agent's edit gets `busy`. Null: never.
		std::function<bool()> gesture_active;
	};

	// scene.node.create/delete/duplicate/reparent/rename, component.add/remove/set, transform.set. A node is named by a
	// ref as in node.get; every edit is one commit of the scene, recorded under the caller's origin. An optional
	// `expected_generation` (the argument, else cmd::call::expected_generation) that is not the scene's gives `stale`.
	// Inside a prefab instance only component.set and transform.set work: they write override records.
	[[nodiscard]] std::vector<registration> register_scene_commands(cmd::command_registry& commands, scene_command_sources sources);
}
