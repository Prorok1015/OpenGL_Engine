#pragma once
#include "edt_asset_usages.h"
#include <snk/model_codec/res_tag.h>
#include <snk/resource/res_file_change.h>
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
	class document;

	enum class asset_operation : std::uint8_t { rename, move, remove };

	std::string_view asset_operation_name(asset_operation operation);

	// `asset` is a file or a folder of the project's own layer. A plan holds one operation: one rename (a file or a folder), or
	// moves or deletes of any number of files and folders.
	struct asset_request
	{
		asset_operation operation = asset_operation::rename;
		res::tag asset;
		// rename: the new file or folder name; move: the folder below res:// it goes into ("" is the root); remove: not used.
		std::string to;
	};

	// Stable, for an agent to branch on.
	enum class refusal_code : std::uint8_t
	{
		// Not produced any more (#1209): kept for the codes already written down.
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
		// Requests of more than one operation in one plan.
		mixed_operations,
		// Two requests give one file two different results (a file inside a folder moved elsewhere).
		overlapping_requests,
		// A folder moved into itself or a folder inside it.
		into_itself,
		// The folder is also a folder of a lower layer: its files there would stay visible under the old path.
		folder_in_lower_layer,
		// A delete of a folder that holds files the project does not list, or a folder that holds a link (never followed or moved).
		folder_has_foreign_entries,
		// More files or bytes than asset_ops_limits allows in one operation.
		too_many_files,
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
		// folder_has_foreign_entries: the entries that are not the project's (res:// tags); folder_in_lower_layer: the files of
		// the lower layer that would stay.
		std::vector<std::string> entries;
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

	// One per referrer file, with every asset of the plan it refers to rewritten in turn.
	struct planned_rewrite
	{
		// Where the file is once the moves are done (a referrer the plan moves is rewritten at its new place).
		std::string file;
		std::filesystem::path root;
		std::string relative;
		std::string before_hash;
		std::string after_text;
		std::vector<std::string> pointers;
		// Where the file is before the moves (`relative` when it does not move).
		std::string source;
	};

	struct plan_item
	{
		// For a file of a requested folder: that file, with the folder request's operation and `to`.
		asset_request request;
		// The requested folder (res:// tag) the file goes with; empty for a file asked by itself.
		std::string folder;
		// Empty for a remove.
		res::tag target;
		std::filesystem::path root;
		std::vector<planned_move> moves;
		// remove: only the places outside the removed files.
		std::vector<asset_usage> referrers;
		// A referrer of several items is rewritten once, in the first item that refers to it.
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
		asset_operation operation = asset_operation::rename;
		// The project's own layer: every move and folder step is below it.
		std::filesystem::path root;
		std::vector<plan_item> items;
		// Files of a renamed or moved folder that are no asset (hidden, temporary, a .resource of nothing): moved as they are.
		std::vector<planned_move> carried;
		// Below the root: the folders to make, each after its parent, and the source folders to remove once empty, each
		// before its parent.
		std::vector<std::string> make_folders;
		std::vector<std::string> remove_folders;
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
		// `from` below `root` was removed into `to` below the journal folder (renamed there, or, across volumes, copied and then
		// removed); `hash` is its content. Not done: `from` is there. Done: `from` is gone and `to` has `hash`.
		stash,
		// The folder `from` below `root` was made (it was not there). Taken back: removed when empty.
		make_folder,
		// The empty folder `from` below `root` was removed. Taken back: made again.
		remove_folder,
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

	// The most a journal on the disk may hold (journal.json and its steps.<n>.json together): a bigger one is not read back.
	inline constexpr std::uint64_t k_journal_read_limit = std::uint64_t{ 16 } << 20;

	// The whole journal as one JSON value. On the disk it is journal.json with its first steps and, past those, the rest in
	// steps.<n>.json ("more_steps": n), so a step costs a write of a bounded file (read_journal puts them together again).
	boost::json::value journal_to_json(const operation_journal& journal);
	// journal.json of <journal_root>/<id>/ and the step files it names as one value journal_from_json reads (a journal of the old
	// form, all its steps in journal.json, reads as it is); nullopt with `reason` when it cannot be read, is no JSON or is bigger
	// than k_journal_read_limit. Read without following a link.
	std::optional<boost::json::value> read_journal(const std::filesystem::path& journal_root, std::string_view id, std::string& reason);
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
		// A folder the operation made that is not empty now: it stays.
		std::vector<std::string> warnings;
	};

	struct asset_ops_manifest
	{
		// The folder the manifest lies in, and its file name.
		std::filesystem::path root;
		std::string file;
		std::vector<manifest_reference> references;
	};

	// What one operation may take at most; a bigger one is refused (too_many_files).
	struct asset_ops_limits
	{
		std::size_t max_files = 4096;
		std::uint64_t max_bytes = 2ull * 1024u * 1024u * 1024u;
		// What the journal may take on the disk, by estimate: half of what is read back, as JSON may grow it.
		std::uint64_t max_journal_bytes = k_journal_read_limit / 2;
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
		// What an operation or its undo did to files of the layers, once it is on the disk and still under the hold: the editor's
		// own change (asset_database::report_self_change), not a delete and a create that somebody else made.
		std::function<void(std::span<const res::file_change>)> report_self_change;
		// Tests: called before each step ("move:<from>", "stash:<from>", "rewrite:<file>"), before it is written to the journal; a
		// non-empty answer fails it. Undoing or rolling back a move or delete step calls it first with
		// "before_undo:<from>"; an undo calls it with "unstash:<from>" before it brings a removed file back; the copy
		// that replaces a rename (see force_copy) calls it with "stash_copied:<from>" / "unstash_copied:<from>" once
		// the copy is read and checked, before it is flushed and put in place.
		std::function<std::string(std::string_view)> fail_step;
		// Tests: asked with "stash:<from>" (a delete moving the file into the journal) and "unstash:<from>" (an undo moving it back);
		// true makes the rename report a move to another volume (the one failure a copy takes over from), so the file is copied,
		// flushed and checked instead.
		std::function<bool(std::string_view)> force_copy;
		// Tests: asked with the same labels before the rename; a non-empty answer is a rename that failed for another reason (a
		// link, a denied access): the step fails and nothing is copied.
		std::function<std::string(std::string_view)> rename_error;
		asset_ops_limits limits;
		// Right before the first step (the journal has its id and folder, no step yet), so the history lists the operation while it
		// runs and a crash leaves it to be undone at the next start. A non-empty answer (or a throw) is why it could not be listed:
		// the operation is refused, nothing done. Then once it ended: `done`, or rolled back (the journal is empty when every step
		// went back, and keeps all its steps when one could not: an undo skips the ones already back).
		std::function<std::string(const operation_journal&)> operation_started;
		std::function<void(const operation_journal&, bool done)> operation_ended;
	};

	// Whether `layer` of the asset database's layers is the engine's own (never changed by an operation).
	bool is_engine_layer(const std::vector<std::filesystem::path>& layers, std::size_t layer, const std::filesystem::path& engine_base);

	// The port is_open_with_unsaved_changes from what the editor knows of one asset: `open` is the open document that has it as its
	// source (null: none), `scene` the editor's scene document, `is_editor_copy` whether the asset is the open level's editor copy.
	// An operation would lose the unsaved edits of a dirty document; the open level, clean or not, cannot take a change of its file
	// from outside yet (US-60-C, #1188).
	bool is_open_with_unsaved_changes(const document* open, const document& scene, bool is_editor_copy);

	class asset_ops
	{
	public:
		explicit asset_ops(asset_ops_ports ports);

		// Reads the disk, changes nothing. A folder is walked in the project's own layer without following a link: each file
		// with an asset record is planned as one asset, the others go along as they are (a delete refuses them).
		operation_plan plan(std::span<const asset_request> requests) const;
		// Does what `plan` found, in this order: the originals of the referrers into the journal, the folders made, the moves,
		// the rewrites, the emptied folders removed; any failure takes everything back.
		operation_report execute(const operation_plan& plan, const execute_options& options = {}) const;
		// Puts back what `journal` records, when none of its files changed since; a step recorded but never done (a crash) is
		// skipped, the ones before it are put back. The journal is not trusted: its folder must be journal_root/<id>, its roots
		// the project's own (top) layer or the manifest's folder, its names ones the project lists, a rewrite of a desc or the manifest.
		undo_report undo(const operation_journal& journal) const;
		// The folders an operation may have changed files in: the project's own (top) layer and the manifest's. The only roots a
		// journal may name (the history checks the journals it reads against them).
		std::vector<std::filesystem::path> changeable_roots() const;
		// `text` with the host's folders written as the project knows them (a layer is "res://", the journal folder
		// "cache://asset_ops", the manifest's folder "<project>", the engine's "<engine>"): what an agent is told of an error.
		std::string without_host_paths(std::string text) const;

	private:
		// What planning one asset learned that the rewrites of its referrers need.
		struct item_facts
		{
			// The asset's res:// tag as the database spells it, and its path below the layer.
			std::string path;
			std::string relative;
			// Its ID as text; empty: none.
			std::string guid;
		};

		std::optional<refusal> plan_into(std::span<const asset_request> requests, operation_plan& out) const;
		// `to`: the destination below res:// a folder request gives the file (nullopt: its own request says).
		std::optional<refusal> plan_one(plan_item& item, const std::optional<std::string>& to, usage_index& usages, item_facts& facts) const;
		// One planned_rewrite per referrer file of `items` (renames and moves).
		std::optional<refusal> plan_rewrites(std::vector<plan_item>& items, const std::vector<item_facts>& facts) const;
		// Tells report_self_change what `steps` did (`undoing`: what taking them back did).
		void announce(const std::vector<journal_step>& steps, bool undoing) const;
		void execute_into(const operation_plan& plan, const execute_options& options, operation_report& report, bool& started) const;
		// Takes the steps of report.journal back, newest first, and writes the journal again: emptied when every step went back,
		// else with all its steps (the ones that did not go back are in rollback_failures).
		void roll_back(operation_report& report) const;

		asset_ops_ports m_ports;
	};
}
