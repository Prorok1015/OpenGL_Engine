#pragma once
#include <snk/scene/scn_input_actions_desc.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <memory>
#include <string>
#include <string_view>

namespace ds { class app_data_storage; }
namespace inp { class action_map; }

namespace gs
{
	// The project's game actions (manifest field `input_actions`, WORLD_BUS.md 4.1): loads the desc, rebuilds the map
	// when the file changes on disk and keeps the last good map while a broken edit is fixed. gs::route_input hands the
	// map to the worlds. Main thread.
	class SNK_API input_actions
	{
	public:
		explicit input_actions(res::resource_system& resources);
		~input_actions();
		input_actions(const input_actions&) = delete;
		input_actions& operator=(const input_actions&) = delete;

		// Follows the open project; called every frame before gs::route_input.
		void update(const ds::app_data_storage& data);
		// The same for a tag given directly; an empty tag drops the map.
		void update(std::string_view tag);

		// Null while there is no tag, or until the desc has loaded once.
		std::shared_ptr<const inp::action_map> map() const { return m_map; }

	private:
		void drop();

		res::resource_system& m_resources;
		std::string m_tag{};
		res::res_handle<scn::input_actions_desc> m_handle{};
		std::shared_ptr<const inp::action_map> m_map{};
		bool m_changed = false;
		bool m_built = false;
		bool m_reported = false;
	};
}
