#pragma once
#include "edt_registration.h"
#include <snk/command/cmd_registry.h>
#include <filesystem>
#include <functional>
#include <vector>

namespace edt
{
	class asset_kind_registry;
	class document_manager;
	class selection_service;
	struct editor_services;

	// A call may ask for at most this many items per page (scene.query, asset.query); the schema refuses more.
	inline constexpr int state_query_max_page_size = 200;
	inline constexpr int state_query_default_page_size = 50;

	// What the read-only state commands look at. Everything is a reference; the caller keeps it alive as long as the
	// returned registrations.
	struct state_command_sources
	{
		document_manager& documents;
		selection_service& selection;
		// The desc system and the assembler of the game layer, taken at each call (editor_services).
		const editor_services& services;
		asset_kind_registry& asset_kinds;
		// The physical roots behind res://, lowest layer first, as the resource system mounts them (the asset
		// browser lists the same merged view). Null: no assets.
		std::function<std::vector<std::filesystem::path>()> asset_layers;
	};

	// Read-only commands over the editor's state, for scripts and agents. Nothing here edits a document or the
	// selection. Each returns the registrations it made; a refused command is left out.
	//
	// ref = {context, path}: the JSON form of ecs::entity_ref. `context` is the name of the world, `path` the node ids
	// from the world root as 16-char hex strings (an empty path is the world root). Inside a prefab instance the path
	// goes on through the ids of the prefab's nodes. A "<level>/<world>" context is read too; replies use the world name.
	//
	// scene.query {document?, world?, filter?: {name_glob?, has_component?[], under?: ref}, page_size?, cursor?}
	//   -> {document, world, generation, items: [{ref, name, components[], prefab?}], next_cursor?}
	//   Nodes of the world desc in depth-first order (nodes inside a prefab instance are listed by node.get). name_glob
	//   is case-insensitive with * and ?, without them a substring; has_component names must all be present (a key or
	//   a type). page_size is 1..state_query_max_page_size (default state_query_default_page_size). The cursor is
	//   opaque and bound to the document and the generation: after an edit it answers `stale`, for another filter
	//   invalid_args.
	// node.get {ref, components?[]} -> {ref, document, generation, world, name, parent, in_instance, prefab?, transform?,
	//   components: {key: desc-json with __type}, children: [{ref, name, components[], has_children}], child_count,
	//   children_truncated, overrides[]}; not_found for a ref that leads nowhere
	// component.schema {type} -> {type, version, kind, is_component, schema}; schema is a JSON Schema generated from the
	//   type_schema of the desc type; not_found for a type without one
	// asset.query {glob?, kind?, directory?, page_size?, cursor?} -> {items: [{tag, kind, size, layer}], next_cursor?,
	//   scan_truncated}; the merged view of the resource layers (a file of an upper layer shadows the same path below),
	//   sorted by path. The cursor is keyset-based (the last path), so it stays valid when assets change.
	// selection.get {} -> {document, generation, items: [{ref, name?, readonly}]} (no item when nothing is selected)
	[[nodiscard]] std::vector<registration> register_state_commands(cmd::command_registry& commands, const state_command_sources& sources);
}
