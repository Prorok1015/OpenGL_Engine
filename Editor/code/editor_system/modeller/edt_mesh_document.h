#pragma once
#include "edt_desc_file_commands.h"
#include "edt_mesh_bake.h"
#include "edt_mesh_io.h"
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace res
{
	class resource_system;
}

namespace edt
{
	struct editor_services;
	class editor_events;

	// What the preview shows of the working mesh, for the window's status line.
	struct mesh_stats
	{
		std::size_t vertices = 0;
		std::size_t faces = 0;
		std::size_t triangles = 0;
		std::size_t skipped_faces = 0;
	};

	// A mesh opened for editing (EPIC-77, #1011): the working copy of an `x.poly.desc` source, the baked geometry of it that
	// the preview draws, and the save that writes both the source and the `x.geom.desc` + `x.geom.bin` pair the game loads.
	// No new file format: the files are the ones the shape tools make. A previewable_document, so the document_manager opens
	// it like the prefab editor's and Save All, the Unsaved list and the close questions see it.
	class mesh_document final : public previewable_document
	{
	public:
		explicit mesh_document(editor_services& services);
		~mesh_document() override;

		// `source` is the `x.poly.desc` or the `x.geom.desc` beside it (a geometry file without a poly file has no source to edit and
		// is refused). Closed, with last_error(), when the file cannot be read.
		void open(const res::tag& source) override;
		void close() override;
		bool is_open() const override { return m_open; }
		const std::string& last_error() const { return m_error; }

		std::string_view kind() const override { return "mesh"; }
		const std::string& title() const override { return m_title; }
		std::string display_title() const { return is_dirty() ? m_title + " [*]" : m_title; }
		const res::tag& source() const override { return m_poly_tag; }
		const res::tag& geometry_file() const { return m_geometry_tag; }
		std::uint64_t generation() const override { return m_generation; }

		// The edits are not at the source's file, or a save wrote the source and not yet the geometry.
		bool is_dirty() const override { return m_open && (m_dirty || m_geometry_stale); }
		bool can_save() const override { return m_open; }
		// Writes the source, then the geometry. Refuses, writing nothing, while the project may not be written, in Play, for a file
		// of a lower layer or of the open level, for a file changed on disk since it was read and for a mesh with no triangle. When the
		// source was written and the geometry was not, the document stays dirty and geometry_stale() says what the game still loads.
		save_result save() override;
		bool edits_locked() const override { return m_locked && m_locked(); }
		void bind_events(editor_events* events) override { m_events = events; }

		// A save wrote the source and not the geometry: the game loads the old geometry until the next Save works.
		bool geometry_stale() const { return m_open && m_geometry_stale; }
		// The text of the Problems row for that case; empty when the geometry is current.
		const std::string& stale_geometry_message() const { return m_stale_message; }

		bool changed_on_disk() const { return m_open && m_changed_on_disk; }
		// Takes the source as the file holds it now and drops the edits. The error text, empty when it worked.
		std::string revert();
		std::string reload() { return revert(); }
		// Keeps the edits and writes them over the file as it is now.
		save_result overwrite();

		void set_files(desc_file_layout files) { m_files = std::move(files); }
		void set_lock_source(std::function<bool()> locked) { m_locked = std::move(locked); }
		// How the geometry pair is written; empty text is success. Null: mesh::write_mesh_geometry. The tests put a failing one here.
		using geometry_writer = std::function<std::string(res::resource_system&, const res::tag&, const rnd::geometry_desc&)>;
		void set_geometry_writer(geometry_writer writer) { m_write_geometry = std::move(writer); }

		// The working mesh; read-only. Null while closed.
		const mesh::poly_mesh* working() const { return m_open ? &m_working.mesh : nullptr; }
		// Changes the working mesh: `change` gets it and says whether it changed anything. The preview follows. False: closed,
		// locked, or nothing changed.
		bool edit(const std::function<bool(mesh::poly_mesh&)>& change);

		// The tag the preview's mesh node draws: the working mesh baked, stored in memory and signalled on every change.
		const res::tag& preview_geometry() const { return m_preview_tag; }
		// Empty when the working mesh bakes to something that can be drawn and saved; else why not (no triangles).
		const std::string& preview_problem() const { return m_preview_problem; }
		const mesh_stats& stats() const { return m_stats; }

	private:
		bool load_source(std::string& error);
		void rebake();
		std::string working_text() const;
		void announce();
		void watch_source();
		void unwatch_source();
		void on_source_changed(const res::tag& changed);
		std::string geometry_file_problem() const;

		editor_services& m_services;
		editor_events* m_events = nullptr;
		desc_file_layout m_files;
		std::function<bool()> m_locked;
		geometry_writer m_write_geometry;

		mesh::poly_mesh_desc m_working;
		mesh::baked_mesh m_baked;
		mesh_stats m_stats;
		std::string m_preview_problem;
		res::tag m_poly_tag;
		res::tag m_geometry_tag;
		res::tag m_preview_tag;
		std::string m_title;
		std::string m_error;
		std::string m_saved_text;
		std::string m_disk_version;
		std::string m_stale_message;
		std::uint64_t m_generation = 0;
		res::resource_system* m_watching = nullptr;
		bool m_open = false;
		bool m_dirty = false;
		bool m_geometry_stale = false;
		bool m_changed_on_disk = false;
	};
}
