#pragma once
#include "edt_asset_database.h"
#include "edt_asset_ops.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The project's history of file operations (EPIC-57 section 6.4, #775): each executed asset operation is one entry that
// Edit > Undo Asset Action takes back through asset_ops::undo. The journals themselves are asset_ops's, in
// cache://asset_ops/<id>/journal.json, but the list of which of them are the editor's own is kept apart, in the editor's per-user
// data (asset_history_file), never in the project: a journal that a cloned project's cache folder brings, or that anybody else
// leaves there, is not in that list and is never undone. After a restart the list is read again, for as long as the journals are.
namespace edt
{
	// "Rename sky.desc", "Move sky.desc", "Delete sky.desc": from the journal alone.
	std::string asset_operation_label(const operation_journal& journal);

	// <settings>/asset_history/<key of the journal folder>.json, where <settings> is the person's settings folder: the last edt://
	// root, else `user_data_dir` (as migration_notice_file). Empty when the journal folder is empty.
	std::filesystem::path asset_history_file(const std::vector<std::filesystem::path>& edt_roots, const std::filesystem::path& user_data_dir,
		const std::filesystem::path& journal_root);

	class asset_history
	{
	public:
		// The newest entries kept; older ones stay on the disk and are ignored.
		static constexpr std::size_t max_entries = 20;
		// A failed undo keeps its entry (the person fixes what blocked it and tries again) this many times, then forgets it.
		static constexpr std::size_t max_undo_attempts = 3;
		// What a journal is renamed to when its entry was forgotten: the restart does not bring it back, the bytes stay.
		static constexpr std::string_view dropped_journal_file = "journal.dropped.json";

		// `journal_root` is the folder cache://asset_ops/ is; `list_file` the editor's own list of its journals (asset_history_file;
		// empty: the list lives in memory for this run only). `serial` numbers the entries (asset_database::next_history_serial).
		asset_history(std::filesystem::path journal_root, std::filesystem::path list_file, std::function<std::uint64_t()> serial);

		// Reads the list and the journals it names again: the newest max_entries, oldest first. Only a journal that is in the list,
		// whose content is what was listed, that passes journal_from_json (not trusted) and names only `roots` (asset_ops::changeable_roots:
		// the project's own layer and the manifest's folder) is an entry. A journal in the folder that is not listed is ignored with a
		// warning. The label of an entry is the list's. Returns how many entries there are.
		std::size_t restore(const std::vector<std::filesystem::path>& roots);
		// An operation that was done (operation_report::journal, already on the disk): listed in the person's data. It replaces the
		// entry of the same id record_pending made.
		void record(operation_journal journal);
		// An operation about to take its first step (asset_ops_ports::operation_started): listed, and saved, as interrupted before
		// anything is changed, so a crash in the middle leaves it to be undone at the next start. Its journal on the disk is then not
		// checked against a hash (it was still being written), only by journal_from_json and the roots.
		void record_pending(const operation_journal& journal);
		// The operation ended (asset_ops_ports::operation_ended): done, it is an ordinary entry; taken back, it is forgotten, unless
		// steps could not be taken back (they stay to be undone).
		void end_pending(const operation_journal& journal, bool done);
		// Whether the newest entry is an operation that did not end (a crash, or steps a rollback could not take back).
		bool next_is_interrupted() const;

		std::size_t size() const { return m_entries.size(); }
		std::optional<std::string> next_label() const;
		std::optional<std::uint64_t> next_serial() const;
		// Whether the newest operation was done after the newest fix of the asset database: the one Edit > Undo takes. Their serials come from
		// asset_database::next_history_serial; none for an empty list.
		static bool newer_than(std::optional<std::uint64_t> operation, std::optional<std::uint64_t> fix)
		{
			return operation.has_value() && (!fix.has_value() || *operation > *fix);
		}
		// The id of the operation undo_last would take back.
		std::optional<std::string> next_operation_id() const;
		// The journal's folder is <root>/<its id>, the id spelled exactly: the only folder set aside or deleted.
		bool is_journal_folder(const operation_journal& journal) const;
		// Takes back the newest operation. Put back: forgotten, its journal folder deleted. Refused (a file changed since or is open
		// with unsaved changes: nothing was touched) or stopped half way (the error says which files): the entry stays, with
		// `keep` set, and the next call tries it again, the steps already put back being skipped; after max_undo_attempts failures
		// it is forgotten and its journal set aside.
		asset_fix_result undo_last(const asset_ops& operations);

	private:
		struct entry
		{
			std::uint64_t serial = 0;
			std::string label;
			// Of the journal as asset_ops wrote it: a journal read back is the editor's own only if it is still that.
			std::string hash;
			std::size_t attempts = 0;
			operation_journal journal;
			bool interrupted = false;
		};

		bool inside_root(const std::filesystem::path& directory) const;
		void set_aside(const operation_journal& journal) const;
		void save() const;

		std::filesystem::path m_root;
		std::filesystem::path m_list_file;
		std::function<std::uint64_t()> m_serial;
		std::vector<entry> m_entries;
	};
}
