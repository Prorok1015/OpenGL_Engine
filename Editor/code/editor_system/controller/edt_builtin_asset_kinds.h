#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_asset_kind_registry.h>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	// What the built-in asset kinds need from the rest of the editor. Plain callbacks, so the kinds can be
	// registered and dispatched without a level or ImGui.
	struct asset_kind_hooks
	{
		std::function<void(const res::tag&)> open_prefab;
		// Loads the desc and adds it to the active world as a new node.
		std::function<void(const res::tag&)> add_desc_to_scene;
		// Where the project's desc files are (what an Apply of the prefab checks before it writes); null: a prefab cannot be
		// written. The members have initializers: a test that builds the hooks with only the first two keeps compiling.
		std::function<desc_file_layout()> desc_layout = {};
		// Play runs: the prefab cannot be edited or applied. Null: never.
		std::function<bool()> edits_locked = {};
		// Opens a desc file as a data document (the schema inspector). Null: the "shader_desc" and "material_desc" kinds are not
		// registered.
		std::function<void(const res::tag&)> open_desc = {};
		// A texture desc (.txm.desc): the texture window, or the read-only viewer for one of a lower layer (#1205). Null: the
		// "texture_2d_desc" kind is not registered.
		std::function<void(const res::tag&)> open_texture = {};
		// A picture file: the read-only viewer with Create Texture (#1205). Null: the "picture.*" kinds are not registered.
		std::function<void(const res::tag&)> open_picture = {};
	};

	// The picture files a double click shows (#1205): the kind "picture.<ext>" for each suffix ".<ext>".
	inline constexpr std::string_view k_picture_suffixes[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".pct", ".raw" };

	// The id of the preview that makes the prefab_document (open_prefab names it, so a module's preview with a longer suffix
	// cannot take its place).
	inline constexpr std::string_view k_prefab_preview_id = "prefab";

	// "prefab" (.prefab.desc: double-click opens the prefab editor; its preview is the prefab_document, registered as an
	// asset preview with the same id), "shader_desc" (.shader.desc) and "material_desc" (.mat.desc, #1198), each opened as a
	// data document, "texture_2d_desc" (.txm.desc) and "picture.png" ... (k_picture_suffixes) when their hooks are set, and
	// "desc" (any asset dropped onto the viewport is added to the scene). The kinds stay registered while the returned handles live.
	[[nodiscard]] std::vector<registration> register_builtin_asset_kinds(asset_kind_registry& registry, asset_kind_hooks hooks);

	// The kinds whose suffix ends with ".desc", as {id, suffix}: what Create Document names a type's files by when the
	// kind's id is the desc type ("shader_desc" -> ".shader.desc", #1263; document_creator::ports::desc_kinds).
	std::vector<std::pair<std::string, std::string>> desc_kind_suffixes(const asset_kind_registry& registry);
}
