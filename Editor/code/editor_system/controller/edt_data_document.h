#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_history.h>
#include <snk/resource/res_tag.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace desc { class desc_system; class schema_registry; struct type_schema; }
namespace res { class resource_system; }

namespace edt
{
	struct editor_services;

	struct open_result
	{
		bool opened = false;
		std::string error;
		// The file is not meant to be a data document (the open level, a type without a schema or that is no document type, a
		// file of a lower layer): nothing is wrong with it, so a double click is no reason for a warning window.
		bool unsuitable = false;

		static open_result ok() { return { true, {}, false }; }
		static open_result failure(std::string message) { return { false, std::move(message), false }; }
		static open_result not_a_document(std::string message) { return { false, std::move(message), true }; }
	};

	// A desc file of a type with a schema and is_document (EPIC-56, #791) edited as data. The document keeps what the file
	// says (`own`: __type, __parent and only the fields set there) apart from what it inherits (the chain of __parent files
	// merged as JSON), so a save writes `own` and never the flattened result of the desc adapter. Views and edits go
	// through schema_ui::compose / edit_component like the components of the Inspector.
	class data_document final : public document
	{
	public:
		// `files`: where the project's desc files are (what desc.write asks before it replaces one is asked here too).
		data_document(editor_services& services, desc_file_layout files);
		~data_document() override;
		data_document(const data_document&) = delete;
		data_document& operator=(const data_document&) = delete;

		// Reads the file (fresh bytes, not the cache), migrates it in memory and follows __parent. Only a res:// file of the
		// project's own top layer that is not the open level's (read_desc_file) is taken, and a parent is literally res://
		// (what validate_desc_tree writes). A failure names the file and the JSON pointer and leaves the document closed.
		open_result open(const res::tag& file);
		bool is_open() const { return m_open; }

		std::string_view kind() const override { return "data"; }
		const std::string& title() const override { return m_title; }
		const res::tag& source() const override { return m_source; }
		bool is_dirty() const override { return m_open && !(m_own == m_saved_own); }
		std::uint64_t generation() const override { return m_generation; }
		bool can_save() const override { return m_open; }
		save_result save() override;
		// Where document_changed goes (set by the document_manager): announced when what the document shows changed
		// without an edit of its own (a parent file was written, the file itself was changed by someone else).
		void bind_events(editor_events* events) override { m_events = events; }

		// The file was changed by someone else (an agent through desc.write, an editor, a VCS) and the document holds
		// unsaved edits, or the file is gone: save() refuses until the document is opened again. A clean document
		// reads the new file by itself.
		bool changed_on_disk() const { return m_changed_on_disk; }
		// Reads the file again and drops the unsaved edits (the person's choice: the file changed on disk and they take
		// its version). A failure (the file is gone, no longer a data document...) names the file and leaves the document as
		// it was.
		open_result reload();
		// Writes the document over a file that changed on disk (the person's choice, never an agent's: nothing registers a
		// command for it): the version on disk is taken as the base, then the same save() runs with every check (project's
		// top layer, not the open level, validate_desc_tree, size). A refusal leaves the document as it was, still flagged.
		save_result save_over();
		// Moves when a version of the file other than the ones seen before is found on disk: a banner the person put off
		// ("Later") comes back for the next outside change, not for the same one signalled again.
		std::uint64_t disk_change_serial() const { return m_disk_changes; }
		// Why the parents could not be read again after one of them changed (the last good ones stay shown); empty: none.
		const std::string& chain_problem() const { return m_chain_problem; }

		const std::string& type() const { return m_type; }
		// The schema of the document's type (null when it is gone, e.g. the game layer that registered it was replaced).
		const desc::type_schema* schema() const;
		// The files the document inherits from, the nearest first (empty without __parent).
		const std::vector<res::tag>& parents() const { return m_parents; }
		const boost::json::object& own() const { return m_own; }
		// The values the file inherits, header keys left out (empty without __parent).
		const boost::json::object& inherited() const { return m_inherited; }
		// What the document has: the schema defaults with the inherited values and `own` on top / without `own`.
		boost::json::value shown() const;
		boost::json::value base() const;

		// `draw` edits a copy of what is shown; what it changed goes into `own` as the pointers that differ from the base.
		// True when `own` changed.
		bool edit(const std::function<bool(boost::json::value& shown)>& draw);
		// Puts a snapshot of `own` back (the history's undo and redo); the document is dirty when it differs from the saved
		// one. False, and nothing changed, for a snapshot of another __type or __parent: what the document inherits would
		// not follow it.
		bool restore_own(boost::json::object own);

		// Undo history (S56-5): the snapshots of `own` before and after each edit, one record per edit of the person. An edit
		// is applied at once, and recorded when the window says no widget is active any more (commit_pending), so dragging
		// a value is one record. next_undo_entry/undo/redo, save, a reload and the loss of the window's focus all commit what
		// is pending first: an edit in the middle of a drag is neither lost nor folded into the next one. A load of the file
		// (open, reload, a clean document reading an outside change) is a boundary: the history starts again.
		bool undo() override;
		bool redo() override;
		std::optional<history_entry> next_undo_entry() const override;
		std::optional<history_entry> next_redo_entry() const override;
		// Writes the edits made since the last record as one record; nothing when there are none.
		void commit_pending();
		// Records the step from `before` to `after` (what commit_pending does with the pending edit). The history's own door:
		// the tests use it to record a snapshot the document cannot take back.
		void record_change(const boost::json::object& before, const boost::json::object& after);
		// `own` differs from the last record (an edit the history has not taken yet).
		bool has_pending_edit() const { return m_open && !(m_own == m_committed_own); }
		const document_history& history() const { return m_history; }

	private:
		struct loaded
		{
			boost::json::object own;
			boost::json::object inherited;
			std::string type;
			std::string version;
			std::vector<res::tag> parents;
		};

		open_result load(const res::tag& file, loaded& out) const;
		open_result resolve_chain(const res::tag& file, const boost::json::object& own, const std::string& type,
			boost::json::object& inherited, std::vector<res::tag>& parents) const;
		void apply(const res::tag& file, loaded&& state);
		void watch_files();
		void sync_watches(const std::vector<res::tag>& wanted);
		void unwatch_files();
		void on_changed(const res::tag& tag);
		void announce() const;
		save_result write(bool over);
		void flag_changed(const std::string& disk_version);
		open_result read_file(const res::tag& file, boost::json::object& out, std::string& version, desc_file_read* checks = nullptr) const;

		editor_services& m_services;
		desc_file_layout m_files;
		res::tag m_source;
		std::string m_title;
		std::string m_type;
		boost::json::object m_own;
		boost::json::object m_saved_own;
		// `own` as of the last history record (or the load): what the next record's `before` is.
		boost::json::object m_committed_own;
		document_history m_history;
		boost::json::object m_inherited;
		// code_version_of the bytes `own` was read from or last written as: a save refuses a file that is not this.
		std::string m_version;
		std::uint64_t m_generation = 0;
		bool m_open = false;
		bool m_changed_on_disk = false;
		std::string m_flagged_version;
		std::uint64_t m_disk_changes = 0;
		std::string m_chain_problem;
		std::vector<res::tag> m_parents;
		editor_events* m_events = nullptr;
		// The resource system the watchers are on (the base one, which outlives every document) and what is watched.
		res::resource_system* m_watching = nullptr;
		std::vector<res::tag> m_watched;
	};
}
