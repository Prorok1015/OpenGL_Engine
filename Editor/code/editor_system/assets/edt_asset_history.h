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
// Edit > Undo Asset Action takes back through asset_ops::undo. The entries are the journals asset_ops writes to
// cache://asset_ops/<id>/journal.json, so the list is rebuilt from them after a restart for as long as the cache is intact.
namespace edt
{
	// "Rename sky.desc", "Move sky.desc", "Delete sky.desc": from the journal alone, so an entry read back from the disk has it too.
	std::string asset_operation_label(const operation_journal& journal);

	class asset_history
	{
	public:
		// The newest entries kept; older ones stay on the disk and are ignored.
		static constexpr std::size_t max_entries = 20;
		// What a journal is renamed to when its entry was refused or forgotten: the restart does not bring it back, the bytes stay.
		static constexpr std::string_view dropped_journal_file = "journal.dropped.json";

		// `journal_root` is the folder cache://asset_ops/ is. `serial` numbers the entries (asset_database::next_history_serial).
		asset_history(std::filesystem::path journal_root, std::function<std::uint64_t()> serial);

		// Reads <journal_root>/*/journal.json again: the newest max_entries, oldest first. A journal that does not parse, has no
		// step, or names a root that is not in `roots` (the layers and the manifest's folder) is left alone and not listed.
		// Returns how many entries there are.
		std::size_t restore(const std::vector<std::filesystem::path>& roots);
		// An operation that was done (operation_report::journal, already on the disk).
		void record(operation_journal journal);

		std::size_t size() const { return m_entries.size(); }
		std::optional<std::string> next_label() const;
		std::optional<std::uint64_t> next_serial() const;
		// The id of the operation undo_last would take back.
		std::optional<std::string> next_operation_id() const;
		// Takes back the newest operation and forgets it: put back, refused (a file changed since: nothing was touched, and
		// the journal is set aside), or stopped half way (the error says which files). Its journal folder is deleted only when
		// it was put back.
		asset_fix_result undo_last(const asset_ops& operations);

	private:
		struct entry
		{
			std::uint64_t serial = 0;
			std::string label;
			operation_journal journal;
		};

		bool inside_root(const std::filesystem::path& directory) const;
		void set_aside(const operation_journal& journal) const;

		std::filesystem::path m_root;
		std::function<std::uint64_t()> m_serial;
		std::vector<entry> m_entries;
	};
}
