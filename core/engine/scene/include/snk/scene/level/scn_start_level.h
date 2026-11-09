#pragma once
#include <snk/scene/level/scn_level_desc.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/common/snk_api.h>
#include <optional>
#include <string>
#include <string_view>

namespace ds { class app_data_storage; }

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
	// from it when it is there and reports the outcome either way.
	class SNK_API start_level_loader
	{
	public:
		enum class state { idle, loading, loaded, failed };

		// `base_resources`: where the level is looked up while the manager has no game layer.
		start_level_loader(context_manager& contexts, res::resource_system& base_resources);

		void begin(const res::tag& level);
		state poll();

		const res::tag& level() const { return m_level; }

	private:
		context_manager& m_contexts;
		res::resource_system& m_base_resources;
		res::tag m_level;
		res::res_handle<scn::level_desc> m_handle;
		state m_state = state::idle;
	};
}
