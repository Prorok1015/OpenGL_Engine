#pragma once
#include "edt_asset_usages.h"
#include <snk/model_codec/res_tag.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Rename, move and delete of project assets under res:// (EPIC-57 section 6.4, #775): the core, with no UI and no command.
// The files are moved below the project's own layer with platform::beneath, not through resource_system::rename/remove,
// so the file_operations flag of the res:// mount stays off for everybody else; the resource system and the asset
// database see the move the way they see a git mv (the .resource travels with the file: a pair by guid).
namespace edt
{
	class asset_database;

	enum class asset_operation : std::uint8_t { rename, move, remove };

	std::string_view asset_operation_name(asset_operation operation);

	struct asset_request
	{
		asset_operation operation = asset_operation::rename;
		res::tag asset;
		// rename: the new file name; move: the folder below res:// the file goes to ("" is the root); remove: not used.
		std::string to;
	};

	// Stable, for an agent to branch on.
	enum class refusal_code : std::uint8_t
	{
		one_asset_only,
		no_database,
		database_not_ready,
		not_res_path,
		not_found,
		engine_layer,
		not_top_layer,
		meta_in_other_layer,
		bad_name,
		no_such_folder,
		unportable_path,
		no_change,
		case_only_rename,
		target_exists,
		binary_name_lost,
		usage_scan_skipped,
		referrer_open,
		has_referrers,
		stale_plan,
		io_error,
		// The desc's binary file is in a lower layer, a save of it is in flight, or something else refers to it.
		binary_pair_blocked,
		// A file that refers to the asset cannot be rewritten (a file of a lower layer, a reference whose guid is another
		// asset's, a file not laid out as the editor writes it): the reference would break, so nothing is done.
		unrewritable_references,
	};

	std::string_view refusal_code_name(refusal_code code);

	struct rewrite_failure
	{
		std::string file;
		// engine_layer, lower_layer, unknown_file, unreadable, not_canonical, pointer_missing, not_a_reference, guid_mismatch.
		std::string code;
		std::string reason;
	};

	struct refusal
	{
		refusal_code code = refusal_code::io_error;
		std::string reason;
		// has_referrers: who refers to the asset; referrer_open: the open file.
		std::vector<asset_usage> referrers;
		// usage_scan_skipped: the desc files that were not read.
		std::vector<std::string> skipped;
		// unrewritable_references: the files that were not rewritten and why.
		std::vector<rewrite_failure> unrewritable;
	};

	struct file_move
	{
		// res:// tags. A removed file "moves" into the journal folder: cache://asset_ops/<id>/files/<path>.
		std::string from;
		std::string to;
	};

	struct rewritten_file
	{
		// A res:// tag, or the manifest's file name.
		std::string file;
		std::vector<std::string> json_pointers;
	};

	// A file the operation will move (the asset, its .resource, for a desc its .bin and .bin.resource), below the root.
	struct planned_move
	{
		std::string from;
		// Below the root; for a remove, below the journal folder.
		std::string to;
		std::string hash;
	};

	struct planned_rewrite
	{
		std::string file;
		std::filesystem::path root;
		std::string relative;
		std::string before_hash;
		std::string after_text;
		std::vector<std::string> pointers;
	};

	struct plan_item
	{
		asset_request request;
		// Empty for a remove.
		res::tag target;
		std::filesystem::path root;
		std::vector<planned_move> moves;
		std::vector<asset_usage> referrers;
		std::vector<planned_rewrite> rewrites;
		std::vector<rewrite_failure> could_not_rewrite;
		// Path-only references that stay as they are: they will not find the asset.
		std::vector<asset_usage> broken_references;
		std::vector<std::string> warnings;
		// remove only: the usages are not all known (a desc too big to read, the database still reading); `force` goes on.
		std::optional<refusal> scan_problem;
		// remove only: there are referrers, `force` is needed.
		bool needs_force = false;
	};

	struct operation_plan
	{
		std::optional<refusal> refused;
		std::vector<plan_item> items;
	};

	struct execute_options
	{
		// Only for a remove: goes on with referrers (they stay, and are reported as broken) or an unread scan.
		bool force = false;
	};

	enum class journal_step_kind : std::uint8_t
	{
		// `from` -> `to` below `root`; `hash` is the content, which the move does not change.
		move,
		// `from` below `root` was replaced: `hash` the original, `after_hash` the new text, `original` (below the journal
		// folder) holds the original bytes.
		rewrite,
		// `from` below `root` was removed after a copy to `to` below the journal folder; `hash` is its content.
		stash,
	};

	struct journal_step
	{
		journal_step_kind kind = journal_step_kind::move;
		std::string root;
		std::string from;
		std::string to;
		std::string hash;
		std::string after_hash;
		std::string original;
	};

	// Everything undo needs, in the order the steps were done. Plain data: it is written as journal.json beside the bytes it
	// refers to, in <journal_root>/<id>/.
	struct operation_journal
	{
		std::string id;
		asset_operation operation = asset_operation::rename;
		// Absolute folder of the journal.
		std::string directory;
		std::vector<journal_step> steps;
	};

	boost::json::value journal_to_json(const operation_journal& journal);
	// Why `journal` cannot be trusted as it is (empty: its shape is fine): an id that is not one safe name, a step path that is
	// absolute, has '..', is a name the project does not list (the check of a live operation) or is not where its kind keeps it,
	// a rewrite of anything but a desc or the manifest. A journal read from a disk is data, never instructions.
	std::string journal_problem(const operation_journal& journal);
	// nullopt: not a journal, or journal_problem found one (`reason` says which).
	std::optional<operation_journal> journal_from_json(const boost::json::value& value, std::string& reason);
	std::optional<operation_journal> journal_from_json(const boost::json::value& value);

	struct operation_report
	{
		bool ok = false;
		std::optional<refusal> refused;
		// A step failed: why, after everything done was taken back (see rollback_failures).
		std::string error;
		std::vector<file_move> moved;
		std::vector<rewritten_file> rewritten;
		std::vector<rewrite_failure> could_not_rewrite;
		std::vector<std::string> warnings;
		std::vector<asset_usage> broken_references;
		// Steps that could not be taken back after a failure: the project is left half done there.
		std::vector<std::string> rollback_failures;
		std::string operation_id;
		operation_journal journal;
	};

	struct undo_report
	{
		bool ok = false;
		std::string error;
		// Files that are not as the operation left them: nothing was touched.
		std::vector<std::string> changed_since;
		// Files the undo would touch that are open with unsaved changes (the `is_open_with_unsaved_changes` port): nothing was touched.
		std::vector<std::string> open_with_unsaved_changes;
		// Steps that were recorded and never done (a crash between the record and the action) are left out.
		std::vector<std::string> restored;
		std::vector<std::string> failures;
	};

	struct asset_ops_manifest
	{
		// The folder the manifest lies in, and its file name.
		std::filesystem::path root;
		std::string file;
		std::vector<manifest_reference> references;
	};

	struct asset_ops_ports
	{
		// Null: no project, no database.
		std::function<const asset_database*()> assets;
		std::function<const desc::desc_system*()> descs;
		std::function<std::optional<asset_ops_manifest>()> manifest;
		// resource/base_path: the engine layer of res:// (empty: none). Files of it are never changed.
		std::filesystem::path engine_base;
		// Where the journals go: the folder cache://asset_ops/ is.
		std::filesystem::path journal_root;
		// A dirty open document, or the open level, is the file: the operation would lose its edits or its place.
		std::function<bool(const res::tag&)> is_open_with_unsaved_changes;
		// Held for the length of an operation (resource_system::hold_changes), so the watcher reports it as one batch.
		std::function<std::shared_ptr<void>()> hold_changes;
		// Tests: called before each step ("move:<from>", "stash:<from>", "rewrite:<file>"), before it is written to the journal; a
		// non-empty answer fails it.
		std::function<std::string(std::string_view)> fail_step;
	};

	// Whether `layer` of the asset database's layers is the engine's own (never changed by an operation).
	bool is_engine_layer(const std::vector<std::filesystem::path>& layers, std::size_t layer, const std::filesystem::path& engine_base);

	class asset_ops
	{
	public:
		explicit asset_ops(asset_ops_ports ports);

		// One request for now (more: one_asset_only). Reads the disk, changes nothing.
		operation_plan plan(std::span<const asset_request> requests) const;
		// Does what `plan` found: the asset first, then the files that refer to it; any failure takes everything back.
		operation_report execute(const operation_plan& plan, const execute_options& options = {}) const;
		// Puts back what `journal` records, when none of its files changed since; a step recorded but never done (a crash) is
		// skipped, the ones before it are put back. The journal is not trusted: its folder must be journal_root/<id>, its roots
		// the project's own (top) layer or the manifest's folder, its names ones the project lists, a rewrite of a desc or the manifest.
		undo_report undo(const operation_journal& journal) const;

	private:
		std::optional<refusal> plan_one(plan_item& item) const;
		void execute_into(const operation_plan& plan, const execute_options& options, operation_report& report) const;
		// Takes the steps of report.journal back, newest first; the journal is emptied when all of them went.
		void roll_back(operation_report& report) const;
		// The folders an operation may have changed files in: the project's own (top) layer and the manifest's.
		std::vector<std::filesystem::path> changeable_roots() const;

		asset_ops_ports m_ports;
	};
}
