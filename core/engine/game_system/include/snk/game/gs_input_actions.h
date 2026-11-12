#pragma once
#include <snk/scene/scn_input_actions_desc.h>
#include <snk/input/inp_user_bindings.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <filesystem>
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
		// The same, with the project name that names the file of the player's bindings (see set_user_folder).
		void update(std::string_view tag, std::string_view project_name);

		// The map the worlds read: the project's with the player's bindings applied. Null while there is no tag, or until the
		// desc has loaded once. The ids of its actions are the project's, whatever the player changed.
		std::shared_ptr<const inp::action_map> map() const { return m_map; }
		// The project's map as written, for a UI that offers "reset". Null when map() is.
		std::shared_ptr<const inp::action_map> project_map() const { return m_project; }

		// The player's bindings live in `folder`/<project name>.json (#736); an empty folder keeps them in memory only. They
		// are read when the project opens and written after each change.
		void set_user_folder(std::filesystem::path folder);
		const inp::user_bindings& user_bindings() const { return m_user; }
		inp::rebind_result rebind(std::string_view action, std::size_t slot, inp::action_binding binding);
		bool reset_binding(std::string_view action);
		void reset_all_bindings();

	private:
		void drop();
		void load_user();
		void rebuild_effective();
		void save_user();

		res::resource_system& m_resources;
		std::string m_tag{};
		res::res_handle<scn::input_actions_desc> m_handle{};
		std::shared_ptr<const inp::action_map> m_map{};
		std::shared_ptr<const inp::action_map> m_project{};
		inp::user_bindings m_user{};
		std::filesystem::path m_user_folder{};
		std::filesystem::path m_user_file{};
		std::string m_project_name{};
		bool m_changed = false;
		bool m_built = false;
		bool m_reported = false;
	};
}
