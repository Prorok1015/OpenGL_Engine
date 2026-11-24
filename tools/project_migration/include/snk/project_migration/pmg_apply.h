#pragma once
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/project_migration/pmg_scan.h>
#include <boost/json/object.hpp>
#include <cstddef>
#include <cstdint>
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
		// "engine": loaded and saved by the engine, as desc_resave does; "json": the migrated JSON, for a .resource
		// sidecar or a file the engine cannot load or whose type would not keep its __parent (the reason is among the
		// notes); "manifest": project.snkproj, after the project steps, with the stamp of the migration.
		std::string written_as;
		std::vector<std::string> notes;
	};

	// What a migration recorded of a file it created (#1346): the hash (journal_hash) and size of the last text it wrote
	// there. A rollback deletes the file only while it still has both; an empty hash is an entry of a journal of format 1,
	// deleted unchecked.
	struct created_check
	{
		std::string hash;
		std::uint64_t size = 0;
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
		// Files a project step created (EPIC-58 S5) and, parallel to them, what was last written there, journaled before
		// each write: a rollback deletes such a file only while it is unchanged.
		std::vector<std::string> created_files;
		std::vector<created_check> created_checks;
		// The created files a rollback kept because they changed after the migration wrote them, each named with what to do.
		std::vector<std::string> rollback_warnings;
		// The project steps that ran, in order, and what they said; project.snkproj adds them to applied_steps.
		std::vector<std::string> project_steps;
		std::vector<std::string> project_notes;
		// Relative to the project root; empty when nothing was written.
		std::string backup;
		// Why the migration was rolled back.
		std::string error;
		// The size of the backup once the migration ended (#767): what report.md shows and --migrate-prune frees.
		std::uintmax_t backup_bytes = 0;
		std::size_t backup_files = 0;
	};

	// The regular files below `folder` and their bytes, found without following a link; what cannot be read counts as
	// nothing.
	struct tree_size
	{
		std::uintmax_t bytes = 0;
		std::size_t files = 0;
	};
	tree_size size_of_tree(const std::filesystem::path& folder);
	// "512 B", "12.3 KiB", "4.0 MiB", "1.2 GiB".
	std::string size_text(std::uintmax_t bytes);

	// Writes `text` to <root>/<relative> inside the project; the error, nullopt on success. prj::write_project_file by default.
	using project_writer = std::function<std::optional<std::string>(const std::filesystem::path& root, const std::filesystem::path& relative,
		std::string_view text)>;

	// Where apply_migration asks apply_context::stop_requested: before each planned file (and the pre steps), before the
	// post steps, and last before project.snkproj (`commit`).
	enum class apply_checkpoint { file, post_steps, commit };

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
		// Where the plan's project steps are found; a plan with project steps is refused without them.
		const project_step_registry* project_steps = nullptr;
		// Runs `fn` on the main thread and returns once it ran; false when the main thread refuses (it is going away),
		// which fails the migration like a failed write. Only the engine's load and save of a desc go through it (they
		// wait on resource handles, a main-thread call). Empty: they run on the calling thread.
		std::function<bool(const std::function<void()>& fn)> on_main;
		// True: the migration is cancelled there and rolled back like a failed write. The `commit` answer is the last:
		// after a false one the migration is committed and no stop is asked again (a later failure still rolls back).
		// Empty: never stopped.
		std::function<bool(apply_checkpoint where)> stop_requested;
	};

	struct apply_outcome
	{
		// Why nothing was written: a newer or invalid file, a manifest that is not current enough to read. Empty otherwise.
		std::string refused;
		migration_report report;
	};

	// Why apply_migration refuses `plan` before writing anything: a project.snkproj that is newer or invalid, an invalid
	// file without plan.force, or plan.refused. Empty when it may run. A path or a reason from the project's files in it went
	// through journal_path_text (#991), so the editor shows the message with text_escape::escaped_message.
	std::string plan_refusal(const migration_plan& plan);

	// Why a project with these content roots is not migrated: its rollback would refuse them (check_journal_paths'
	// rule). plan_migration puts it in plan.refused, so the plan and a dry run show it; apply_migration checks again.
	std::optional<std::string> content_roots_refusal(const std::vector<std::filesystem::path>& content_roots);

	// The files a migration of `plan` leaves as they are and reports: unknown_type and newer ones (never migrated), and
	// the invalid ones plan.force passes over.
	std::vector<skipped_file> skipped_by(const migration_plan& plan);

	// The report as report.md has it.
	std::string report_text(const migration_report& report);

	// The format of report.json this engine writes: 2 has journal_format and created_files entries {path, hash_after,
	// size_after}; a journal without journal_format is format 1 (created_files as plain paths). A newer one is refused.
	inline constexpr std::int64_t k_journal_format = 2;

	// The report as report.json (the journal) has it.
	boost::json::object report_to_json(const migration_report& report);

	// Refuses what plan_refusal names, and content roots its own rollback would refuse (check_journal_paths' rule); a newer
	// file is left as it is (partial). An empty plan writes nothing (status ok, no id). Otherwise creates the
	// journal (status in_progress; its folder is taken exclusively, so two processes never share an id) before the first
	// change and backs every file up, project.snkproj included. Then the pre project steps, the files, the post project
	// steps, and last project.snkproj with engine_version and migrated_with of this engine and the steps added to
	// applied_steps (EPIC-58 4.1); each written file is checked again from the disk. Any failure puts back every file it
	// wrote, checked by its hash, and deletes the created files that are unchanged (a changed one is kept, with a warning
	// in report.rollback_warnings): rolled_back, or rollback_failed when one could not be
	// put back. With context.on_main it may run on a worker thread (everything else it does is safe there, EPIC-58 4.5).
	apply_outcome apply_migration(const migration_plan& plan, const apply_context& context);

	// Ids of the migrations a person has to look at: the journal still says in_progress (the process stopped in the
	// middle) or rollback_failed.
	std::vector<std::string> unfinished_migrations(const std::filesystem::path& root);

	// A folder below .snk/migrations and what its report.json says (#767). `problem` names why the journal cannot be
	// trusted (it cannot be read, has no status, or no `created` of the form 2026-01-31T12:00:00Z); what could be read
	// is still filled in.
	struct migration_folder
	{
		std::string id;
		std::string status;
		std::string created;
		std::string problem;
	};
	// Real folders only, a link is not one; sorted by id.
	std::vector<migration_folder> migration_folders(const std::filesystem::path& root);

	struct rollback_context
	{
		// prj::write_project_file by default.
		project_writer write;
		std::function<void()> pause_watch;
		std::function<void()> resume_watch;
		// Set to true once the rollback wrote anything (the journal's in_progress first): an error while it is still false
		// changed nothing. Null: not told.
		bool* wrote = nullptr;
		// journal_hash of report.json as the caller checked it: a journal that reads otherwise now is refused before any
		// write. Empty: not checked.
		std::string journal_hash;
		// Set to the warnings of the created files the rollback kept (changed after the migration), as report.json's
		// rollback_warnings has them; a rollback with kept files still succeeds. Null: not told.
		std::vector<std::string>* warnings = nullptr;
	};

	// The hash a journal (and every file of it) is compared by: 16 hex digits.
	std::string journal_hash(std::string_view bytes);

	// A path (or any text) of a journal as a message or a question shows it: a journal is a file anyone can edit, so it goes
	// through text_escape::escaped_text (#991): a backslash doubled, a line break, carriage return and tab as \n \r \t, any
	// other hidden code point (controls, bidi, invisible and fill characters, variation selectors, tags, spaces other than
	// U+0020) as \u{202E}, a byte that is no UTF-8 as \x{FF}; cut at 8 KiB, never inside a code point or an escape ("..."
	// then). check_journal_paths refuses a path over 1024 bytes, so a journal's path is always shown whole.
	std::string journal_path_text(std::string_view path);

	// The paths a journal (.snk/migrations/<id>/report.json) names, as a rollback reads them.
	struct journal_paths
	{
		std::string backup;
		// files[].path ("" for an entry without a string path).
		std::vector<std::string> files;
		// created_files[]: the path of each entry, and parallel to it the hash and size it was written with (an empty
		// hash for a plain path of a format 1 journal). One path is listed once.
		std::vector<std::string> created;
		std::vector<created_check> created_checks;
	};

	// Reads what `journal` (a parsed report.json) names, as every rollback and its listing do: the error, nullopt when it
	// reads. Refused: a journal_format that is no whole number or is newer than k_journal_format ("written by a newer
	// engine"); a created_files that is not a list; an entry of it that is neither a path nor an object with a string
	// path, a hash_after of 16 lowercase hex digits and a whole size_after; a path listed twice with another hash or size.
	std::optional<std::string> parse_journal_paths(const boost::json::object& journal, journal_paths& paths);

	// Why a rollback of migration `id` must not touch what `journal` names (#910); nullopt when every path passes. The
	// first bad path is named (through journal_path_text) with the reason:
	// - backup is exactly .snk/migrations/<id>/backup, as apply_migration writes it, whether or not anything is restored;
	// - the root is a folder and .snk/migrations/<id> is a folder all the way down, no link on the way;
	// - the content roots come from the backup's project.snkproj when the journal lists it, else from the current one (a
	//   rollback is a recovery tool: the project need not open); neither reads as a manifest: refused, naming the backup
	//   to restore by hand. Each root is a relative folder inside the project, not the root itself, and its first folder
	//   is no reserved one (below);
	// - every path is at most 1024 bytes and a chain of plain names below the root; a file of files[] is project.snkproj or
	//   lies inside a content root of that manifest, a created file lies inside a content root;
	// - and none lies below a first folder prj::is_code_ignored_dir refuses (any '.' name, build, out, ...) or one of
	//   prj::own_top_dir_names (code, .snk, .cache, build), whatever a content root says.
	std::optional<std::string> check_journal_paths(const std::filesystem::path& root, std::string_view id, const journal_paths& journal);

	// Reads the journal of migration `id` and checks it with parse_journal_paths and check_journal_paths: the refusal (or
	// why it does not read), nullopt when a rollback may touch what it names. The editor offers a rollback at open only for
	// a journal that passes.
	std::optional<std::string> check_journal(const std::filesystem::path& root, std::string_view id);

	// Restores the backup of migration `id` and deletes the files it created. A file already back to its original is
	// left as it is, so a rollback that stopped half way can be run again. Refuses before writing anything (naming the
	// files) when a file is neither what the migration wrote nor the original, or the journal fails parse_journal_paths
	// or check_journal_paths. A created file with a hash is deleted only while its size and hash are the journal's: one
	// that is gone is passed over, a changed one (or one too big or unreadable to compare) is kept and named in
	// report.json's rollback_warnings and context.warnings, and the rollback still succeeds (#1346). A plain path of a
	// format 1 journal is deleted unchecked. The error; nullopt on success.
	std::optional<std::string> rollback_migration(const std::filesystem::path& root, std::string_view id, const rollback_context& context = {});
}
