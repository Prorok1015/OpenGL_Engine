#pragma once
#include "edt_desc_file_commands.h"
#include <snk/editor/edt_document.h>
#include <snk/resource/res_tag.h>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace desc { class desc_system; class schema_registry; struct type_schema; }

namespace edt
{
	struct editor_services;

	struct open_result
	{
		bool opened = false;
		std::string error;

		static open_result ok() { return { true, {} }; }
		static open_result failure(std::string message) { return { false, std::move(message) }; }
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

		const std::string& type() const { return m_type; }
		const boost::json::object& own() const { return m_own; }
		// The values the file inherits, header keys left out (empty without __parent).
		const boost::json::object& inherited() const { return m_inherited; }
		// What the document has: the schema defaults with the inherited values and `own` on top / without `own`.
		boost::json::value shown() const;
		boost::json::value base() const;

		// `draw` edits a copy of what is shown; what it changed goes into `own` as the pointers that differ from the base.
		// True when `own` changed.
		bool edit(const std::function<bool(boost::json::value& shown)>& draw);
		// Puts a snapshot of `own` back (undo and redo, S56-5); the document is dirty when it differs from the saved one.
		// False, and nothing changed, for a snapshot of another __type or __parent: what the document inherits would
		// not follow it.
		bool restore_own(boost::json::object own);

	private:
		const desc::type_schema* schema() const;
		open_result read_file(const res::tag& file, boost::json::object& out, std::string& version, desc_file_read* checks = nullptr) const;

		editor_services& m_services;
		desc_file_layout m_files;
		res::tag m_source;
		std::string m_title;
		std::string m_type;
		boost::json::object m_own;
		boost::json::object m_saved_own;
		boost::json::object m_inherited;
		// code_version_of the bytes `own` was read from or last written as: a save refuses a file that is not this.
		std::string m_version;
		std::uint64_t m_generation = 0;
		bool m_open = false;
	};
}
