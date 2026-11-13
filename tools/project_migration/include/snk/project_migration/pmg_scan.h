#pragma once
#include <snk/desc/desc_system.h>
#include <snk/project/prj_version.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a project holds and what a migration of it would do, without writing anything (EPIC-58 S2, #752).
namespace prj::migration
{
	enum class file_class
	{
		current,
		outdated,
		newer,
		// A type nobody registers in the game layer (a module that is not loaded): the file is left as it is.
		unknown_type,
		invalid,
		// An asset file without its <name>.resource sidecar (EPIC-57 creates them). A sidecar itself is read with
		// res::read_asset_meta: current, newer (written by a newer engine) or invalid.
		missing_resource,
	};

	std::string_view to_string(file_class kind);

	// One object with a __type, where it is in its file.
	struct typed_object
	{
		std::string pointer;
		std::string type;
		int version = 1;
		// The game layer's current version; 0 for an unknown type.
		int current = 0;
		bool known = true;
	};

	struct scanned_file
	{
		// Relative to the project root, with '/'.
		std::string path;
		file_class kind = file_class::current;
		std::vector<typed_object> objects;
		std::vector<std::string> unknown_types;
		// Why the file is invalid or newer; empty otherwise.
		std::string reason;
	};

	struct manifest_state
	{
		int format_version = 0;
		std::string engine_version;
		std::optional<prj::version_match> engine;
		file_class kind = file_class::current;
		std::string reason;
	};

	struct scan_options
	{
		// A file larger than this is invalid and is not read.
		std::uintmax_t max_file_bytes = std::uintmax_t{ 64 } << 20;
	};

	struct scan_result
	{
		manifest_state manifest;
		std::vector<scanned_file> files;

		std::size_t count(file_class kind) const;
		const scanned_file* find(std::string_view path) const;
	};

	// The class of a desc file's text, as scan_project gives it (a file just written is checked with it). `path` is kept as given.
	scanned_file classify_desc(std::string path, std::string_view text, const desc::desc_system& game_layer);

	// Reads project.snkproj under `root` and every file of its content roots (not the engine's layer; hidden folders,
	// links and other non-regular files are not followed). `game_layer` decides which types are known and their versions.
	// Writes nothing.
	scan_result scan_project(const std::filesystem::path& root, const desc::desc_system& game_layer, const scan_options& options = {});

	// Leaves (JSON pointers of non-container values) added, removed and changed by a migration.
	struct diff_stat
	{
		std::size_t added = 0;
		std::size_t removed = 0;
		std::size_t changed = 0;
	};

	struct step_use
	{
		std::string type;
		int from_version = 0;
		std::string summary;
		std::size_t objects = 0;
	};

	struct file_plan
	{
		std::string path;
		std::vector<step_use> steps;
		std::vector<desc::migration_warning> warnings;
		diff_stat diff;
		// Line diff of the pretty JSON before and after, with plan_options::full_diff.
		std::optional<std::string> full_diff;
	};

	struct plan_options
	{
		bool full_diff = false;
		scan_options scan;
	};

	struct migration_plan
	{
		// The scan the plan was made from; a file whose migration failed in memory is invalid here.
		scan_result scan;
		std::vector<file_plan> files;
		// Every step the plan runs, over the whole project.
		std::vector<step_use> steps;

		bool empty() const { return files.empty(); }
	};

	// Migrates every outdated file of `scan` in memory with `game_layer`. Writes nothing.
	migration_plan plan_migration(const std::filesystem::path& root, const scan_result& scan, const desc::desc_system& game_layer,
		const plan_options& options = {});
}
