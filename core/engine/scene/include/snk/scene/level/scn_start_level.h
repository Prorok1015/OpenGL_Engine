#pragma once
#include <snk/scene/level/scn_level_desc.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/common/snk_api.h>
#include <optional>
#include <string>
#include <string_view>

namespace ds { class app_data_storage; class task_system; }

// The start level of a project (#591): the editor and the shipping host resolve and report it through these, so the
// two cannot drift apart (CI reads "level loaded: <tag>" from both).
namespace scn
{
	class context_manager;

	// `override_level` when it is not empty, else the open project's start_scene; empty when neither names a level.
	SNK_API std::string start_level_name(const ds::app_data_storage& data, std::string_view override_level);
	// A name holding "://" is a tag as written; any other is a path in res://.
	SNK_API res::tag start_level_tag(std::string_view name);
	// start_level_name, then start_level_tag; nullopt when there is no start level.
	SNK_API std::optional<res::tag> resolve_start_level(const ds::app_data_storage& data, std::string_view override_level);

	SNK_API void report_level_loaded(const res::tag& level);
	// An error naming `level`, then each diagnostic of the load behind `handle` (which may be a copy of the level).
	SNK_API void report_level_failed(const res::tag& level, const res::res_handle<scn::level_desc>& handle);

	// Loads a level without blocking a frame: begin() asks for the desc, poll() once a frame builds the set "level"
	// from it when it is there, then waits (settling) until the loads the level started are done: nothing pending in the
	// game layer and the base resources, and no general main-thread continuation queued (#656). GPU uploads are not
	// waited for: the level's data is there, the frames that upload it are not part of "loaded". After
	// scene/level_settle_frames polls in settling it is reported loaded anyway, with one warning naming what is left.
	class SNK_API start_level_loader
	{
	public:
		enum class state { idle, loading, settling, loaded, failed };

		// `base_resources`: where the level is looked up while the manager has no game layer.
		start_level_loader(context_manager& contexts, res::resource_system& base_resources, ds::task_system& tasks);

		void begin(const res::tag& level);
		state poll();

		const res::tag& level() const { return m_level; }

	private:
		// Loads not finished in the game layer and the base resources, plus queued general main-thread continuations.
		std::size_t pending_work() const;

		context_manager& m_contexts;
		res::resource_system& m_base_resources;
		ds::task_system& m_tasks;
		int m_settle_polls = 0;
		res::tag m_level;
		res::res_handle<scn::level_desc> m_handle;
		state m_state = state::idle;
	};
}
