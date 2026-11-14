#pragma once
#include "edt_desc_file_commands.h"
#include "edt_editor_camera.h"
#include <snk/editor/edt_document.h>
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <entt/entt.hpp>

namespace res
{
	class resource_system;
}

namespace scn
{
	class prefab_desc;
}

namespace edt
{
	struct editor_services;
	class editor_events;

	// Marker component — distinguishes prefab preview camera from editor camera
	struct prefab_camera_tag {};

	// A prefab opened in the Prefab Editor: the owner of the set "preview:<tag>" while it is open, with a working copy
	// (a prefab_desc of its own, never the cache's) that edit() changes, apply (save()) writes and revert() takes from the
	// file again (#147, EPIC-25 US-25-2). The state is the editor's: no field was added to an engine type.
	class prefab_document final : public previewable_document
	{
	public:
		explicit prefab_document(editor_services& services);
		~prefab_document() override;

		void open(const res::tag& prefab_tag) override;
		void close() override;

		bool is_open() const override { return m_open; }
		// The working copy differs from what the file holds (or was last written as).
		bool is_dirty() const override { return m_open && m_dirty; }
		std::string_view kind() const override { return "prefab"; }
		const std::string& title() const override { return m_prefab_name; }
		// What the window shows: the name, with "[*]" while there are unapplied edits.
		std::string display_title() const { return is_dirty() ? m_prefab_name + " [*]" : m_prefab_name; }
		const res::tag& source() const override { return m_source_tag; }
		// Moves with every edit and every revert.
		std::uint64_t generation() const override { return m_generation; }

		// Apply: writes the working copy over the prefab's file (see save()).
		bool can_save() const override { return m_open; }
		// Apply. Refuses, writing nothing, while the project may not be written, in Play (edits_locked), when the file is of
		// a lower layer or changed on disk since it was read, and when the file cannot be checked; on success the preview's
		// source is signalled as changed (the levels that use the prefab reload it) and the working copy is the save point.
		save_result save() override;
		bool edits_locked() const override { return m_locked && m_locked(); }
		void bind_events(editor_events* events) override { m_events = events; }

		// The file changed on disk while the working copy has edits (a clean copy takes the new file by itself): Apply is
		// refused until reload() or overwrite().
		bool changed_on_disk() const { return m_open && m_changed_on_disk; }
		// Takes the file as it is now and drops the edits (like revert()). The error text, empty when it worked.
		std::string reload() { return revert(); }
		// Keeps the edits and writes them over the file as it is now.
		save_result overwrite();

		// Where the project's desc files are (the checks of an Apply); without it nothing can be written.
		void set_files(desc_file_layout files) { m_files = std::move(files); }
		// Play runs: the prefab cannot be edited or applied.
		void set_lock_source(std::function<bool()> locked) { m_locked = std::move(locked); }

		// The working copy; null while the document is closed.
		const scn::prefab_desc* working() const { return m_working.get(); }
		// Changes the working copy: `change` gets it and says whether it changed anything; the preview follows (reconcile by
		// node id), the document is dirty when the result differs from the file. False: closed, locked, or nothing changed.
		bool edit(const std::function<bool(scn::prefab_desc&)>& change);
		// Revert: the working copy comes from the file again (fresh bytes, not the cache), the preview follows. The error
		// text, empty when it worked; nothing changes on an error.
		std::string revert();

		entt::registry* get_registry();
		const std::string& prefab_name() const { return m_prefab_name; }
		const res::tag& source_tag() const { return m_source_tag; }
		// The preview is the only context of its own set; the panel binds its input to this target.
		const std::string& set_name() const { return m_set_name; }
		static constexpr std::string_view context_name() { return CONTEXT_NAME; }

		static constexpr std::string_view RT_COLOR = "memory://__prefab_camera_rt";
		static constexpr std::string_view RT_DEPTH = "memory://__prefab_depth_rt";
		static constexpr std::string_view RT_TP_ACCUM = "memory://__prefab_tp_accum_rt";
		static constexpr std::string_view RT_TP_REVEAL = "memory://__prefab_tp_reveal_rt";

	private:
		void inject_camera(entt::registry& reg);
		// The bytes the working copy would be written as (the adapter's form: header, references, pretty JSON).
		std::vector<std::byte> working_bytes() const;
		void sync_preview();
		void watch_source();
		void unwatch_source();
		void on_source_changed(const res::tag& changed);
		void announce();

		editor_services& m_services;
		editor_events* m_events = nullptr;
		desc_file_layout m_files;
		std::function<bool()> m_locked;
		std::unique_ptr<scn::prefab_desc> m_working;
		entt::entity m_root = entt::null;
		std::vector<std::byte> m_saved_bytes;
		// code_version_of the file's bytes when it was read or last written: an Apply refuses a file that is not this.
		std::string m_disk_version;
		std::uint64_t m_generation = 0;
		bool m_changed_on_disk = false;
		res::resource_system* m_watching = nullptr;

		res::tag m_source_tag;
		std::string m_prefab_name;
		std::string m_set_name;
		bool m_open = false;
		bool m_dirty = false;
		editor_camera_state m_camera_state;

		static constexpr std::string_view k_working_tag = "memory://editor/__prefab_working";
		static constexpr std::string_view CONTEXT_NAME = "__prefab_preview";
	};
}
