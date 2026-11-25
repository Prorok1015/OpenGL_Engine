#pragma once
#include "../autosave/edt_autosave_source.h"
#include "edt_desc_file_commands.h"
#include "edt_shader_text.h"
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace res
{
	class resource_system;
}

namespace edt
{
	struct editor_services;
	class editor_events;

	// A shader source (`.vert`, `.frag`, `.geom`, `.glsl`...) of res:// opened for editing (#1202): its text, whether it differs from the file,
	// and the save that writes it back and tells the renderer, which builds the programs that use it again (ShaderManager watches every file
	// of a program). A previewable_document, so the document_manager, Save All, the Unsaved list and the close questions see it like a
	// prefab or a mesh. The window's text widget keeps its own undo; the document holds the text it gave back (`set_text`).
	class shader_source_document final : public previewable_document, public autosave_source
	{
	public:
		explicit shader_source_document(editor_services& services);
		~shader_source_document() override;

		// `source` is a res:// tag of a shader source (is_shader_source). Closed, with last_error(), when the file cannot be read.
		void open(const res::tag& source) override;
		void close() override;
		bool is_open() const override { return m_open; }
		const std::string& last_error() const { return m_error; }

		std::string_view kind() const override { return "shader_source"; }
		const std::string& title() const override { return m_title; }
		std::string display_title() const { return is_dirty() ? m_title + " [*]" : m_title; }
		const res::tag& source() const override { return m_tag; }
		std::uint64_t generation() const override { return m_generation; }

		bool is_dirty() const override { return m_open && m_dirty; }
		bool can_save() const override { return m_open && !read_only(); }
		// Writes the text over the file as it is now and signals the change. Refuses, writing nothing, in Play, while the project may not be
		// written, for a file of a lower layer and for a file somebody else changed since it was read (`changed_on_disk()`).
		save_result save() override;
		save_result save_as(const res::tag&) override { return save_result::failure("a shader source is saved over its own file"); }
		autosave_snapshot autosave_capture() const override;
		bool edits_locked() const override { return m_locked && m_locked(); }
		void bind_events(editor_events* events) override { m_events = events; }

		// The text, with LF line breaks (the file's own line ending and BOM come back on save).
		const std::string& text() const { return m_text; }
		// What the window's widget holds now. Refused (false) while read-only or locked, or when nothing changed.
		bool set_text(std::string text);
		int line_count() const;

		// The file is not the project's own (a layer below it holds it): it can be read and not saved.
		bool read_only() const { return m_open && !m_top_layer; }
		// Why it cannot be edited or saved, ready to show; empty when it can.
		std::string read_only_reason() const;
		// The file had both kinds of line break: Save writes the dominant one everywhere.
		bool mixed_endings() const { return m_open && m_file.mixed_endings; }

		// Somebody changed the file on disk since it was read, and the document has edits.
		bool changed_on_disk() const { return m_open && m_changed_on_disk; }
		// The window's typing met a change made behind its back (a reload of the file, another edit): the person gets the Reload / Overwrite
		// choice of a changed file.
		void raise_conflict();
		// Takes the file as it is on disk and drops the edits. The error text, empty when it worked.
		std::string revert();
		// Keeps the edits and writes them over the file as it is now.
		save_result overwrite();

		void set_files(desc_file_layout files) { m_files = std::move(files); }
		void set_lock_source(std::function<bool()> locked) { m_locked = std::move(locked); }

	private:
		struct located
		{
			std::string relative;
			std::filesystem::path file;
			bool top_layer = false;
			std::string error;
		};
		struct disk_read
		{
			std::vector<std::byte> bytes;
			std::string error;
		};

		located locate(const res::tag& tag) const;
		disk_read read_disk() const { return read_disk(m_tag, m_disk_path); }
		disk_read read_disk(const res::tag& tag, const std::filesystem::path& path) const;
		// Reads and parses `path`; the document's own state changes only when it worked.
		bool load(const res::tag& tag, const std::filesystem::path& path, std::string& error);
		void announce();
		void watch();
		void unwatch();
		void on_source_changed(const res::tag& changed);

		editor_services& m_services;
		editor_events* m_events = nullptr;
		desc_file_layout m_files;
		std::function<bool()> m_locked;
		res::resource_system* m_watching = nullptr;

		bool m_open = false;
		std::string m_error;
		res::tag m_tag;
		std::string m_title;
		std::filesystem::path m_disk_path;
		bool m_top_layer = false;
		shader_text_file m_file;
		std::string m_text;
		std::string m_saved_text;
		std::string m_disk_version;
		bool m_dirty = false;
		bool m_changed_on_disk = false;
		std::uint64_t m_generation = 0;
	};
}
