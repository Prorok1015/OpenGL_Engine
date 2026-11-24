#pragma once
#include "../autosave/edt_autosave_source.h"
#include "edt_desc_file_commands.h"
#include "edt_mesh_bake.h"
#include "edt_mesh_io.h"
#include "edt_mesh_view.h"
#include <snk/editor/edt_document.h>
#include <snk/model_codec/res_tag.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

namespace res
{
	class resource_system;
}

namespace scn
{
	class prefab_desc;
	struct camera_component;
	struct mouse_controller_component;
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
	class mesh_document final : public previewable_document, public autosave_source
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
		// The source's bytes Save would write (#1186): the poly file only, the geometry pair is derived from it.
		autosave_snapshot autosave_capture() const override;
		bool edits_locked() const override { return m_locked && m_locked(); }
		void bind_events(editor_events* events) override { m_events = events; }

		// A save wrote the source and not the geometry: the game loads the old geometry until the next Save works.
		bool geometry_stale() const { return m_open && m_geometry_stale; }
		// The text of the Problems row for that case; empty when the geometry is current.
		const std::string& stale_geometry_message() const { return m_stale_message; }

		// A geometry file is there and is not the bake of the source as the file holds it (found when the source was read: a Discard or a
		// crash between the two writes, or another mesh's file). Save refuses to write over it; overwrite_geometry() does.
		bool geometry_mismatch() const { return m_open && m_geometry_mismatch; }
		const std::string& geometry_mismatch_message() const { return m_mismatch_message; }
		// Save, replacing a geometry file that is not the bake of the source: the person's decision (the window's "Overwrite geometry").
		// Also writes the geometry alone when only the file is out of date.
		save_result overwrite_geometry();

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
		// What the window shows and picks of the working mesh (mode, selection, hover, x-ray). Editor state: not saved, not an edit.
		mesh::mesh_view& view() { return m_view; }
		// Changes the working mesh: `change` gets it and says whether it changed anything. The preview follows. False: closed,
		// locked, or nothing changed.
		bool edit(const std::function<bool(mesh::poly_mesh&)>& change);

		// The tag the preview's mesh node draws: the working mesh baked, stored in memory and signalled on every change.
		const res::tag& preview_geometry() const { return m_preview_tag; }
		// Empty when the working mesh bakes to something that can be drawn and saved; else why not (no triangles).
		const std::string& preview_problem() const { return m_preview_problem; }
		const mesh_stats& stats() const { return m_stats; }

		// The preview's world: its own set, the camera that draws into render_target(), one node with the mesh. Null / empty where no
		// context manager exists (tests of the document alone).
		entt::registry* registry();
		const std::string& set_name() const { return m_set_name; }
		static constexpr std::string_view context_name() { return "__mesh_preview"; }
		const std::string& render_target() const { return m_rt_color; }
		// The resource the preview world was made from (given up with it).
		const res::tag& world_template() const { return m_world_tag; }
		scn::camera_component* camera();
		// The camera's controller: the window turns its input off while a selection gesture runs.
		scn::mouse_controller_component* camera_controller();
		// The preview camera's view-projection as the renderer builds it for the size the camera draws this frame (the picking and the
		// overlay need the same matrix); nullopt without a preview or while the camera is parked (size zero).
		std::optional<glm::mat4> view_projection();
		// The window stops showing this document: its camera draws nothing until it is shown again.
		void park_camera();
		// The render targets of the preview's camera and the template of its world are given up when the document closes: this takes the
		// target tags (the renderer owns the textures, the document does not know it).
		using targets_release = std::function<void(const std::vector<res::tag>&)>;
		void set_targets_release(targets_release release) { m_release_targets = std::move(release); }
		// The game layer goes: the preview world (a set of the context manager) is dropped, the working copy and its edits stay; after it
		// the world is made again in the new layer.
		void release_preview_world();
		void rebuild_preview_world();
		// What the Overwrite geometry question says: the files it writes and, when the geometry file is not the bake of the source, that it is replaced.
		std::string overwrite_geometry_question() const;

	private:
		entt::entity camera_entity();
		save_result save_impl(bool replace_foreign_geometry);
		void check_geometry_match();
		void open_preview_world();
		void close_preview_world();
		// The render targets and the world template, once they were named (also of a world that failed or is parked).
		void release_preview_resources();
		void sync_preview_node();
		bool load_source(std::string& error);
		void rebake();
		std::string working_text() const;
		void announce();
		void watch_source();
		void unwatch_source();
		void on_source_changed(const res::tag& changed);
		std::string geometry_file_problem(bool replace_foreign_geometry) const;

		editor_services& m_services;
		editor_events* m_events = nullptr;
		desc_file_layout m_files;
		std::function<bool()> m_locked;
		geometry_writer m_write_geometry;
		targets_release m_release_targets;

		mesh::poly_mesh_desc m_working;
		mesh::mesh_view m_view;
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
		bool m_geometry_mismatch = false;
		std::string m_mismatch_message;

		std::uint64_t m_serial = 0;
		std::string m_set_name;
		std::string m_rt_color;
		std::string m_rt_depth;
		std::string m_rt_accum;
		std::string m_rt_reveal;
		std::string m_working_tag;
		res::tag m_world_tag;
		std::unique_ptr<scn::prefab_desc> m_preview_prefab;
		entt::entity m_root = entt::null;
		bool m_has_world = false;
	};
}
