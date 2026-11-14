#pragma once
#include <snk/desc/desc_system.h>
#include <snk/project/prj_version.h>
#include <snk/resource/res_asset_meta.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
		// A file that is no .resource has one beside it, paired as the editor's asset database pairs them
		// (res::asset_name_key of its owner); false is what EPIC-57's assets.ids step gives one (#776).
		bool has_resource = true;
		// A desc whose root has a "__guid", whatever its value: what assets.ids_drop_desc_guid takes out (#772).
		bool root_guid = false;
		// The .resource of this folder that pairs with the file (project-relative); empty when the folder has none.
		std::string resource_path;
		// The .resource that pairs with the file does not read (has_resource is true): it has no ID to replace a __guid.
		bool resource_unreadable = false;
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
		// The .resource version the engine reads. Only a test sets another one: it stands for an engine whose
		// res::asset_meta_version was raised with the step under test.
		std::int64_t resource_meta_version = res::asset_meta_version;
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

	// A .resource sidecar as a desc of the pseudo-type resource_meta, known at its root only: its steps are the game
	// layer's migrations of "resource_meta" (EPIC-58 3.4), and what is under "settings" belongs to the importer. A
	// sidecar at the steps' current version must also read with res::read_asset_meta of an engine reading
	// `readable_version`; one the engine does not read is newer.
	scanned_file classify_sidecar(std::string path, std::string_view text, const desc::desc_system& game_layer,
		std::int64_t readable_version = res::asset_meta_version);

	// The steps of resource_meta on a sidecar's root; typed objects under "settings" are neither migrated nor stamped.
	void migrate_sidecar(const desc::desc_system& game_layer, json::object& root, std::string_view file,
		std::vector<desc::migration_warning>* warnings = nullptr);

	// The game layer's types as a scan asks about them (known: registered or with a schema; current_version), taken on the
	// main thread so that a scan can run on another one (#761). A type it does not list is unknown, at version 1, as the
	// game layer answers for it.
	class type_catalog
	{
	public:
		struct entry
		{
			bool known = false;
			int current_version = 1;
		};

		static type_catalog from(const desc::desc_system& game_layer);

		void add(std::string type, entry value);
		bool is_known(std::string_view type) const;
		int current_version(std::string_view type) const;
		const std::map<std::string, entry, std::less<>>& types() const { return m_types; }

	private:
		std::map<std::string, entry, std::less<>> m_types;
	};

	// Reads project.snkproj under `root` and every file of its content roots (not the engine's layer; hidden folders,
	// links and other non-regular files are not followed). `game_layer` decides which types are known and their versions
	// (through type_catalog::from). Writes nothing.
	scan_result scan_project(const std::filesystem::path& root, const desc::desc_system& game_layer, const scan_options& options = {});

	// The same scan with a snapshot of the types: it touches no engine service, so it may run on a worker thread.
	scan_result scan_project(const std::filesystem::path& root, const type_catalog& types, const scan_options& options = {});

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
		// --force (S6): an invalid file is left as it is and reported, instead of refusing the whole migration.
		bool force = false;
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
		// Why apply_migration refuses the plan; empty when it may run.
		std::string refused;
		// scan_options::resource_meta_version of the plan: what apply checks a migrated sidecar against.
		std::int64_t resource_meta_version = res::asset_meta_version;
		// plan_options::force.
		bool force = false;

		// An older engine_version keeps the version dialog.
		bool manifest_outdated() const { return scan.manifest.kind == file_class::outdated; }
		// Only a migration that leaves no file of an unloaded module behind writes this engine's engine_version: a
		// partial one keeps the old one, so the dialog still asks (EPIC-58 4.1).
		bool completes() const { return scan.count(file_class::unknown_type) == 0; }
		// "Nothing to do": no file, no project step, and no engine_version to bring to this engine.
		bool empty() const { return files.empty() && project_steps.empty() && !(manifest_outdated() && completes()); }
	};

	// Migrates every outdated file of `scan` in memory with `game_layer` and lists the project steps it needs. Writes nothing.
	migration_plan plan_migration(const std::filesystem::path& root, const scan_result& scan, const desc::desc_system& game_layer,
		const plan_options& options = {});
}
