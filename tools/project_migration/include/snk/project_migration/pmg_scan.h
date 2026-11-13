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
		// An asset file without its <name>.resource sidecar (EPIC-57 creates them). A sidecar itself is classified as a
		// desc of the pseudo-type resource_meta (classify_sidecar).
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
		// What the last migration wrote (EPIC-58 2): the engine it ran with and every project step that ever ran.
		std::string migrated_with;
		std::vector<std::string> applied_steps;
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

	// A .resource sidecar as a desc of the pseudo-type resource_meta: its steps are the game layer's migrations of
	// "resource_meta" (EPIC-58 3.4). At res::asset_meta_version it must also read with res::read_asset_meta; at another
	// version only the steps know its shape, so only its __type is checked.
	scanned_file classify_sidecar(std::string path, std::string_view text, const desc::desc_system& game_layer);

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

	class project_step_registry;

	struct plan_options
	{
		bool full_diff = false;
		scan_options scan;
		// The project steps to plan (pmg_project_steps.h); none without it.
		const project_step_registry* project_steps = nullptr;
	};

	struct planned_project_step
	{
		std::string id;
		std::string phase;
		std::string summary;
	};

	struct migration_plan
	{
		// The scan the plan was made from; a file whose migration failed in memory is invalid here.
		scan_result scan;
		std::vector<file_plan> files;
		// Every step the plan runs, over the whole project.
		std::vector<step_use> steps;
		// The project steps whose `needed` holds, pre ones first, each phase in the order of the registry.
		std::vector<planned_project_step> project_steps;

		// Whether project.snkproj gets the stamp of a migration: an older engine_version keeps the version dialog.
		bool manifest_outdated() const { return scan.manifest.kind == file_class::outdated; }
		// "Nothing to do": every applied plan writes engine_version, migrated_with and applied_steps (EPIC-58 4.1).
		bool empty() const { return files.empty() && project_steps.empty() && !manifest_outdated(); }
	};

	// Migrates every outdated file of `scan` in memory with `game_layer` and lists the project steps it needs. Writes nothing.
	migration_plan plan_migration(const std::filesystem::path& root, const scan_result& scan, const desc::desc_system& game_layer,
		const plan_options& options = {});
}
