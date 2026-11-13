#pragma once
#include <snk/project_migration/pmg_apply.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The command line of a migration (EPIC-58 4.3, S6, #757): `editor --headless --project <dir> --migrate [--dry-run]
// [--report <file>] [--force]` and `--migrate-rollback <id>`. The editor's main runs these with the game layer of the
// project it opened; the exit code is what they return.
namespace prj::migration
{
	inline constexpr int k_exit_done = 0;
	// --dry-run only: the plan is not empty.
	inline constexpr int k_exit_migration_needed = 1;
	// Bad input, a refused plan (an invalid file without --force, a newer or invalid project.snkproj), a migration left
	// unfinished in the project, a refused rollback.
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
	};

	// The plan as --dry-run prints it.
	std::string plan_text(const migration_plan& plan);

	int run_migrate(const migrate_command& command, const command_context& context);
	int run_migrate_rollback(std::string_view id, const command_context& context);
}
