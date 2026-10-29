#pragma once
#include <snk/command/cmd_types.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The undo history of a document (EPIC-15 #29, #141-#144; AI_INTEGRATION.md §3.4). The document diffs every edit against
// its last committed state and records the difference as history_changes: the history knows no document type. A scene
// keys its changes by node, a data document (#146) will key them by field.
namespace edt
{
	// The difference one edit made, reversible both ways.
	class history_change
	{
	public:
		virtual ~history_change() = default;
		// Brings the document back to before the edit.
		virtual void undo() = 0;
		// Applies the edit again.
		virtual void redo() = 0;
		// What the edit changed (a node, a field): two changes conflict when they share a key.
		virtual const std::vector<std::string>& keys() const = 0;
	};

	struct history_record
	{
		std::uint64_t id = 0;
		std::string label;
		cmd::origin origin;
		// One per edit; more than one when the edits of a group (an agent's session) were folded into one record.
		std::vector<std::unique_ptr<history_change>> changes;
		// The group the record belongs to; 0: none.
		std::uint64_t group = 0;
	};

	struct rollback_result
	{
		// Edits of the group undone.
		std::size_t rolled_back = 0;
		// Edits of the group still applied: the rollback stopped at a conflict before them.
		std::size_t remaining = 0;
		// Where it stopped: the key a later edit of someone else changed too, and that edit's label.
		std::optional<std::string> conflict_key;
		std::string conflict_label;
	};

	// Records are applied up to the current position; undo steps back, redo forward, a new record drops what was undone.
	// A group gathers the edits of one origin (an agent's session) into one record while nobody else edits; an edit of
	// another origin in between starts a new record of the group after it, so undo stays linear. Main thread only.
	class document_history
	{
	public:
		static constexpr std::size_t k_default_limit = 100;

		explicit document_history(std::size_t limit = k_default_limit) : m_limit(limit == 0 ? 1 : limit) {}

		// Older records go first when there are more. At least 1.
		void set_limit(std::size_t limit);
		std::size_t limit() const { return m_limit; }

		// One edit by `origin`; null: nothing changed, nothing recorded. Drops the records that were undone.
		void record(std::string label, const cmd::origin& origin, std::unique_ptr<history_change> change);

		bool can_undo() const { return m_position > 0; }
		bool can_redo() const { return m_position < m_records.size(); }
		// The record undo (redo) would apply; null when there is none.
		const history_record* next_undo() const { return can_undo() ? &m_records[m_position - 1] : nullptr; }
		const history_record* next_redo() const { return can_redo() ? &m_records[m_position] : nullptr; }
		// The edits of the record are undone in reverse order (redone in order); false when there was nothing.
		bool undo();
		bool redo();

		// Forgets every record and every open group: a boundary (a level loaded, the world list changed).
		void clear();
		std::size_t size() const { return m_records.size(); }
		std::size_t position() const { return m_position; }
		const std::vector<history_record>& records() const { return m_records; }

		// The document is as saved now; undo or redo back to this point makes it clean again.
		void mark_saved();
		bool is_at_save_point() const;

		// Opens the group of `origin` (closing an open one of the same origin first) and returns its id.
		std::uint64_t begin_group(std::string label, const cmd::origin& origin);
		// Closes it; the next edit of that origin starts a new record. Unknown or closed: nothing.
		void end_group(std::uint64_t group);
		// The open group of `origin`, 0 when none.
		std::uint64_t open_group(const cmd::origin& origin) const;
		// Undoes the applied edits of the group, newest first, and drops them from the history. Stops before an edit
		// whose key a later applied record of another origin changed too, so a human's edit is never undone or
		// overwritten. The records that were undone (redo) are dropped as well.
		rollback_result rollback_group(std::uint64_t group);

	private:
		static std::string origin_key(const cmd::origin& origin);
		std::uint64_t top_id() const { return m_position > 0 ? m_records[m_position - 1].id : 0; }
		void drop_redo();
		void enforce_limit();

		struct open_group_state
		{
			std::uint64_t id = 0;
			std::string label;
		};

		std::vector<history_record> m_records;
		std::size_t m_position = 0;
		std::size_t m_limit;
		std::uint64_t m_next_id = 1;
		std::uint64_t m_next_group = 1;
		// The id of the record on top when saved; nullopt: no state of the history is the saved one.
		std::optional<std::uint64_t> m_saved = std::uint64_t{ 0 };
		std::map<std::string, open_group_state> m_groups; // by origin
	};
}
