#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/project/prj_version.h>

// project.snkproj: the manifest of a game project (ADR-02, docs/roadmap/design/EPIC-42-PLAN.md §2.1).
namespace prj
{
	inline constexpr int k_manifest_format_version = 1;
	inline constexpr std::string_view k_manifest_file_name = "project.snkproj";

	enum class module_kind
	{
		runtime,
		editor,
	};

	struct module_ref
	{
		std::string name;
		module_kind kind = module_kind::runtime;
		bool optional = false;
	};

	struct manifest
	{
		int format_version = k_manifest_format_version;
		std::string name;
		engine_version engine;
		// A res:// tag, or empty when the manifest names no start scene.
		std::string start_scene;
		// Relative to the project root, highest priority first; the first one receives writes.
		std::vector<std::filesystem::path> content_roots;
		std::vector<module_ref> modules;
		std::vector<std::string> platforms;
	};

	struct manifest_error
	{
		// JSONPath-like location of the offending value ("$.modules[1].kind"); empty for file-level errors.
		std::string json_path;
		std::string reason;
	};

	struct manifest_result
	{
		std::optional<manifest> data;
		// Set exactly when data is empty.
		std::optional<manifest_error> error;
		// Non-fatal findings, such as unknown fields kept for forward compatibility.
		std::vector<std::string> warnings;
	};

	// Strict JSON (no comments, no trailing commas).
	SNK_API manifest_result parse_manifest(std::string_view json);

	// `path` is the manifest file or the project directory holding project.snkproj. Runs before the resource system
	// exists (the manifest decides its mount points), so this is a plain capped file read. Warnings are also logged.
	SNK_API manifest_result load_manifest(const std::filesystem::path& path);

	SNK_API std::string to_string(const manifest_error& error);
}
