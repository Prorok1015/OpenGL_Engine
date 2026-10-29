#pragma once
#include "edt_editor_camera.h"
#include "edt_scene_history.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_history.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_world_desc.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace edt
{
	class editor_events;

	// What a commit does besides marking the document changed.
	enum class commit_mode
	{
		push,       // the edit changed a desc: reload the active world from it
		state_only, // the level was changed in place (a world was added, a level was created): nothing to reload, no document_changed
	};

	// The level being edited: its identity, its world descs, the dirty flag and the single entry point for edits
	// (commit). The level's context set belongs to the context_manager (editor_services::level_set()).
	class scene_document final : public document
	{
	public:
		// Level identity
		res::tag editor_tag;
		res::tag level_tag;
		std::string level_name;
		// __guid of the level file in level_tag; empty until the first save (or when loaded from a file without one).
		std::string level_guid;

		// Multi-world state
		std::vector<scn::world_desc> world_descs;
		std::vector<std::string> world_names;
		std::vector<std::vector<std::string>> worlds_systems_list;
		// -1 (or any index outside world_descs) = the level has no worlds; use the accessors below, never index directly.
		int active_world_idx = 0;
		// Bumped on every change of the edited descs (and of which one is edited); caches of derived views key on it.
		uint64_t desc_generation = 0;
		void touch_descs() { ++desc_generation; }

		// Survives the reload of the editor world, which recreates the camera entity.
		editor_camera_state camera;

		bool has_active_world() const { return active_world_idx >= 0 && active_world_idx < static_cast<int>(world_descs.size()); }
		scn::world_desc* active_world_desc() { return has_active_world() ? &world_descs[active_world_idx] : nullptr; }
		const scn::world_desc* active_world_desc() const { return has_active_world() ? &world_descs[active_world_idx] : nullptr; }
		// Name of the active world as the level knows it, null when there is none.
		const std::string* active_world_name() const
		{
			return has_active_world() && active_world_idx < static_cast<int>(world_names.size()) ? &world_names[active_world_idx] : nullptr;
		}

		// Every edit of the level ends here: bumps the generation, records what the edit changed in the history (the
		// worlds are compared with the last commit by node id, #141), pushes the descs of the worlds it changed to the
		// level (commit_mode::push: the active one through the pusher, the others through the world reloader; an edit
		// that changed nothing pushes nothing) and tells document_changed. The generation grows by exactly one per commit (the pusher must not bump it again).
		// `label` names the edit in the log and the Edit menu. A push that finds nothing to push to (no level loaded) does
		// not announce; commit_mode::state_only never does, and is a boundary of the history (the world list changed).
		void commit(std::string_view label, commit_mode mode = commit_mode::push);
		// Announces document_changed for the current generation; commit does it after the push.
		void notify_changed();
		// The document is as saved: undo or redo back to here makes it clean again (#144).
		void mark_clean();

		// The undo history (#141): commits record into it, undo/redo/rollback below apply it and push like a commit.
		document_history& history() { return m_history; }
		const document_history& history() const { return m_history; }
		bool undo();
		bool redo();
		// The before-apply hook runs only when the rollback undoes something.
		rollback_result rollback_group(std::uint64_t group);
		// Forgets the history and takes the worlds as they are now as its start: after a load, a level reload, the game
		// layer letting go of the level. Dirty stays as it was.
		void reset_history();
		// Takes the worlds as they are now without recording a change (the live transforms written back before a save).
		void rebase_history();
		// Runs before undo, redo or a rollback changes the worlds: pointers into them (the selection) go stale.
		void set_before_history_apply(std::function<void()> fn) { m_before_apply = std::move(fn); }
		// Who makes the edits committed now; edits of an agent are recorded as its (AI_INTEGRATION.md §3.4).
		const cmd::origin& edit_origin() const { return m_origin; }
		cmd::origin exchange_edit_origin(cmd::origin origin) { return std::exchange(m_origin, std::move(origin)); }
		// What the last commit compared and copied.
		const scene_commit_stats& last_commit_stats() const { return m_last_stats; }

		// Set by scene_editor, which knows how to reload a world from its desc. Returns false when it did nothing.
		void set_pusher(std::function<bool()> pusher) { m_pusher = std::move(pusher); }
		// Reloads world `index` (not the active one) from its desc after undo/redo changed it; set by scene_editor.
		void set_world_reloader(std::function<bool(std::size_t index)> reloader) { m_world_reloader = std::move(reloader); }
		// Where document_changed goes; set by document_manager.
		void bind_events(editor_events* events) { m_events = events; }
		// How the level reaches the disk; set by the editor, which owns the level controller.
		void set_save_handlers(std::function<bool()> save, std::function<bool(const res::tag&)> save_as)
		{
			m_save = std::move(save);
			m_save_as = std::move(save_as);
		}

		std::string_view kind() const override { return "scene"; }
		const std::string& title() const override { return level_name; }
		const res::tag& source() const override { return level_tag; }
		bool is_dirty() const override { return m_unrecorded || !m_history.is_at_save_point(); }
		std::uint64_t generation() const override { return desc_generation; }
		bool can_save() const override { return editor_tag.is_valid() && m_save != nullptr; }
		save_result save() override;
		save_result save_as(const res::tag& target) override;

	private:
		scene_history_index::diff_result record_edit(std::string_view label);
		// Runs `apply` (an undo, a redo, a rollback) between the before-apply hook and the reload of the worlds it changed.
		// False when one of them threw: the history was cleared (recover_from_apply).
		bool apply_history(std::string_view what, const std::function<void()>& apply);
		void recover_from_apply(std::string_view what, std::string_view error);
		void finish_apply();
		// Rebuilds the contexts of the worlds changed since the last call; document_changed if any was rebuilt.
		void reload_touched();

		// Changed in a way the history does not hold (a new level, a world added, an edit it could not compare).
		bool m_unrecorded = false;
		document_history m_history;
		scene_history_index m_index;
		scene_commit_stats m_last_stats;
		cmd::origin m_origin = cmd::origin::ui();
		std::function<void()> m_before_apply;
		editor_events* m_events = nullptr;
		std::function<bool()> m_pusher;
		std::function<bool(std::size_t)> m_world_reloader;
		std::function<bool()> m_save;
		std::function<bool(const res::tag&)> m_save_as;
	};

	// Edits committed while it lives are made by `origin` (an agent's session).
	class edit_origin_scope
	{
	public:
		edit_origin_scope(scene_document& document, cmd::origin origin)
			: m_document(document), m_previous(document.exchange_edit_origin(std::move(origin))) {}
		~edit_origin_scope() { m_document.exchange_edit_origin(std::move(m_previous)); }
		edit_origin_scope(const edit_origin_scope&) = delete;
		edit_origin_scope& operator=(const edit_origin_scope&) = delete;

	private:
		scene_document& m_document;
		cmd::origin m_previous;
	};
}
