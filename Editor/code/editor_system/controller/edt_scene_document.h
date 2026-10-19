#pragma once
#include "edt_editor_camera.h"
#include <snk/editor/edt_document.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_world_desc.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scn { class context_manager; class context_set; class ecs_assembler; }
namespace ecs { class system_factory; }

namespace edt
{
	class editor_events;

	// What a commit does besides marking the document changed.
	enum class commit_mode
	{
		push,       // the edit changed a desc: reload the active world from it
		state_only, // the level was changed in place (a world was added, a level was created): nothing to reload
	};

	// The level being edited: its identity, its world descs, the dirty flag and the single entry point for edits
	// (commit). The level's context set is owned by the context_manager and reached through level_set().
	class scene_document final : public document
	{
	public:
		// Service references (set during init, not owned)
		std::weak_ptr<scn::context_manager> ctx_manager;
		scn::ecs_assembler* assembler = nullptr;
		ecs::system_factory* sfactory = nullptr;

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

		// The context set of the level; null while the context manager is gone.
		scn::context_set* level_set() const;

		// Every edit of the level ends here: marks the document dirty, bumps the generation, pushes the descs to the
		// level (commit_mode::push) and tells document_changed. `label` names the edit for the log now and for the
		// Edit menu once edits are undoable. A push that finds nothing to push to (no level loaded) does not announce.
		void commit(std::string_view label, commit_mode mode = commit_mode::push);
		// Announces document_changed for the current generation; commit does it after the push.
		void notify_changed();
		void mark_clean() { m_dirty = false; }

		// Set by scene_editor, which knows how to reload a world from its desc. Returns false when it did nothing.
		void set_pusher(std::function<bool()> pusher) { m_pusher = std::move(pusher); }
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
		bool is_dirty() const override { return m_dirty; }
		std::uint64_t generation() const override { return desc_generation; }
		bool can_save() const override { return editor_tag.is_valid() && m_save != nullptr; }
		save_result save() override;
		save_result save_as(const res::tag& target) override;

	private:
		bool m_dirty = false;
		editor_events* m_events = nullptr;
		std::function<bool()> m_pusher;
		std::function<bool()> m_save;
		std::function<bool(const res::tag&)> m_save_as;
	};
}
