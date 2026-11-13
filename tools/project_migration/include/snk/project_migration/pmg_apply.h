#pragma once
#include <snk/project_migration/pmg_scan.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace res { class resource_system; }

// Applying a migration plan to a project: journal, backup, writes inside the project, check, automatic rollback and
// report (EPIC-58 S3, #753). Everything it writes is below <project>/.snk/migrations/<id>/ or a file the plan migrates.
namespace prj::migration
{
	// rollback_failed: a file could not be put back; the project is partly migrated until rollback_migration succeeds.
	enum class migration_status { in_progress, ok, partial, rolled_back, rollback_failed };

	std::string_view to_string(migration_status status);

	struct applied_file
	{
		std::string path;
		std::vector<step_use> steps;
		std::vector<desc::migration_warning> warnings;
		std::string hash_before;
		// Recorded in the journal just before the file is written, so a rollback after a crash recognizes it either way;
		// empty for a file the migration never reached.
		std::string hash_after;
		// "engine": loaded and saved by the engine, as desc_resave does; "json": the migrated JSON, for a file the engine
		// cannot load or whose type would not keep its __parent (the reason is among the notes).
		std::string written_as;
		std::vector<std::string> notes;
	};

	struct skipped_file
	{
		std::string path;
		file_class kind = file_class::current;
		std::string reason;
		std::vector<std::string> unknown_types;
	};

	struct migration_report
	{
		std::string id;
		std::string created;
		std::string engine_before;
		std::string engine_after;
		migration_status status = migration_status::in_progress;
		std::vector<applied_file> files;
		std::vector<skipped_file> skipped;
		// Files a project step created (EPIC-58 S5): a rollback deletes them.
		std::vector<std::string> created_files;
		// Relative to the project root; empty when nothing was written.
		std::string backup;
		// Why the migration was rolled back.
		std::string error;
	};

	// Writes `text` to <root>/<relative> inside the project; the error, nullopt on success. prj::write_project_file by default.
	using project_writer = std::function<std::optional<std::string>(const std::filesystem::path& root, const std::filesystem::path& relative,
		std::string_view text)>;

	struct apply_context
	{
		std::filesystem::path root;
		// The game layer, the one the plan was made with; res:// of `resources` is the project's content.
		desc::desc_system& descs;
		res::resource_system& resources;
		// The project's file watcher is paused while files are written (resume runs on success and on failure).
		std::function<void()> pause_watch;
		std::function<void()> resume_watch;
		project_writer write;
		// "<UTC yyyymmddThhmmssZ>-<engine version>" by default; a taken name gets "-2", "-3"...
		std::function<std::string()> make_id;
	};

	struct apply_outcome
	{
		// Why nothing was written: a newer or invalid file, a manifest that is not current enough to read. Empty otherwise.
		std::string refused;
		migration_report report;
	};

	// Refuses a plan with newer or invalid files. An empty plan writes nothing (status ok, no id). Otherwise creates the
	// journal (status in_progress; its folder is taken exclusively, so two processes never share an id) before the first
	// change, backs every file up, writes the files and checks each one again from the disk; any failure puts back every
	// file it wrote, checked by its hash, and deletes the created files: rolled_back, or rollback_failed when one could
	// not be put back.
	apply_outcome apply_migration(const migration_plan& plan, const apply_context& context);

	// Ids of the migrations a person has to look at: the journal still says in_progress (the process stopped in the
	// middle) or rollback_failed.
	std::vector<std::string> unfinished_migrations(const std::filesystem::path& root);

	struct rollback_context
	{
		// prj::write_project_file by default.
		project_writer write;
		std::function<void()> pause_watch;
		std::function<void()> resume_watch;
	};

	// Restores the backup of migration `id` and deletes the files it created. A file already back to its original is
	// left as it is, so a rollback that stopped half way can be run again. Refuses before writing anything (naming the
	// files) when a file is neither what the migration wrote nor the original, or a path of the journal is not a
	// relative path inside the project (a created file: inside a content root). The error; nullopt on success.
	std::optional<std::string> rollback_migration(const std::filesystem::path& root, std::string_view id, const rollback_context& context = {});
}
