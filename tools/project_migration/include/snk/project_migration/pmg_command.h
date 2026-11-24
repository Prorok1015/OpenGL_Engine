#pragma once
#include <snk/project_migration/pmg_apply.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// The command line of a migration (EPIC-58 4.3, S6, #757): `editor --headless --project <dir> --migrate [--dry-run]
// [--report <file>] [--force]` and `--migrate-rollback <id>`. The editor's main runs these with the game layer of the
// project it opened; the exit code is what they return.
namespace prj::migration
{
	inline constexpr int k_exit_done = 0;
	// --dry-run only: the plan is not empty.
	inline constexpr int k_exit_migration_needed = 1;
	// Bad input, a refused plan (an invalid file without --force, a newer or invalid project.snkproj), a migration left
	// unfinished in the project, a refused rollback, a game module of the project that is not loaded (main), a report
	// that could not be written after a migration that went through.
	inline constexpr int k_exit_refused = 2;
	// Files left as they are: unknown_type, newer, or invalid with --force.
	inline constexpr int k_exit_partial = 4;
	inline constexpr int k_exit_rolled_back = 5;
	// The automatic or manual rollback did not finish: the project is partly migrated until --migrate-rollback succeeds.
	inline constexpr int k_exit_rollback_failed = 6;

	struct migrate_command
	{
		bool dry_run = false;
		bool force = false;
		// Where the report (or with dry_run the plan) is also written, as it is printed. Never inside the project.
		std::filesystem::path report;
	};

	struct prune_command
	{
		std::size_t keep = 0;
		// Print what would be deleted, delete nothing.
		bool dry_run = false;
	};

	struct command_context
	{
		std::filesystem::path root;
		desc::desc_system& descs;
		res::resource_system& resources;
		const project_step_registry* project_steps = nullptr;
		// The rest only for tests: by default prj::write_project_file, "<UTC>-<engine>", the log (project/migrate) and
		// prj::write_text_atomically.
		project_writer write;
		std::function<std::string()> make_id;
		std::function<void(std::string_view)> print;
		std::function<std::optional<std::string>(const std::filesystem::path& file, std::string_view text)> write_report;
		// Removes a migration folder for --migrate-prune; empty on success, else why not. By default
		// platform::beneath::remove_tree_beneath from `root` (#1316): a link inside is removed as the link.
		std::function<std::string(const std::filesystem::path& folder)> remove_tree;
		// The root of `resources` when that is a layer (the game layer's): it gets the project's asset index for a
		// --migrate (scoped_project_index). Null: `resources` itself.
		res::resource_system* index_root = nullptr;
	};

	// The exit code of a migration that ended with `status`.
	int exit_code_of(migration_status status);

	// The plan as --dry-run prints it.
	std::string plan_text(const migration_plan& plan);

	// The status report.json of migration `id` names; empty when it has none or it cannot be read.
	std::string journal_status(const std::filesystem::path& root, std::string_view id);

	// What a rollback that ended with `error` exits with: k_exit_done without one, k_exit_refused when it wrote nothing
	// (rollback_context::wrote stayed false: a refusal, a retry refused included), else k_exit_rollback_failed (files were
	// put back only in part).
	int rollback_exit_code(const std::optional<std::string>& error, bool wrote);

	int run_migrate(const migrate_command& command, const command_context& context);
	int run_migrate_rollback(std::string_view id, const command_context& context);
	// `--migrate-prune --keep <N> [--dry-run]` (#767): deletes the folders of finished migrations (ok, partial,
	// rolled_back) below .snk/migrations but the newest N by `created`. An unfinished one (in_progress, rollback_failed),
	// one of another status and one whose journal cannot be trusted stay and are not counted; the last are named. The
	// folders to delete are printed before anything is deleted. k_exit_refused when one could not be deleted.
	int run_migrate_prune(const prune_command& command, const command_context& context);
}
