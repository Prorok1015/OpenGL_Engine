#pragma once
#include <snk/scene/scn_input_actions_desc.h>
#include <snk/input/inp_user_bindings.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ds { class app_data_storage; }
namespace inp { class action_map; }

namespace gs
{
	// What rebind did: `result` says whether the binding took; `saved` is false when it did but the file could not be
	// written (the change holds for this run only).
	struct rebind_outcome
	{
		inp::rebind_result result{};
		bool saved = true;
		bool ok() const { return result.ok(); }
	};

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
		// True when map() has a gamepad button or axis binding.
		bool has_gamepad_bindings() const { return m_has_gamepad_bindings; }
		// After update(), each frame: a module built for an API minor before 3.16 reads bindings as keyboard or mouse only, and
		// a visit over them of its own fails on a gamepad one. Warns once per map and set of such modules (the map is not
		// filtered), naming them. Needs the versions the host keeps (app::modules::loaded_module_apis).
		void warn_about_old_modules(const ds::app_data_storage& data);
		// The "gamepad" setting of the desc (the defaults without one), shared by the bindings of a device.
		inp::gamepad_config gamepad_config() const { return m_gamepad; }

		// The player's bindings live in `folder`, one file per project named by the hash of the project name (#736); an empty folder keeps them in memory only. They
		// are read when the project opens and written after each change.
		void set_user_folder(std::filesystem::path folder);
		const inp::user_bindings& user_bindings() const { return m_user; }
		// `allow_conflict`: a binding another action has is put all the same (both keep it, conflicts() names the pair).
		rebind_outcome rebind(std::string_view action, std::size_t slot, inp::action_binding binding, bool allow_conflict = false);
		rebind_outcome remove_binding(std::string_view action, std::size_t slot);
		// The bindings two actions of map() share that the project's own map does not (what the player's changes or an edit of
		// the desc made); rebuilt with the map.
		const std::vector<inp::binding_conflict>& conflicts() const { return m_conflicts; }
		// Changes with every map built (a desc edit, a change of the player's bindings): lets a view rebuild only when needed.
		std::uint64_t revision() const { return m_revision; }
		// False when the action had no change; the file of the others is written again.
		bool reset_binding(std::string_view action);
		void reset_all_bindings();
		// False after a change the file could not take, until the next one that it does.
		bool user_file_saved() const { return m_saved; }

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
		inp::gamepad_config m_gamepad{};
		inp::user_bindings m_user{};
		std::filesystem::path m_user_folder{};
		std::filesystem::path m_user_file{};
		std::string m_project_name{};
		bool m_saved = true;
		bool m_has_gamepad_bindings = false;
		std::vector<inp::binding_conflict> m_conflicts{};
		std::uint64_t m_revision = 0;
		const inp::action_map* m_warned_map = nullptr;
		std::uint64_t m_warned_revision = ~std::uint64_t{ 0 };
		bool m_changed = false;
		bool m_built = false;
		bool m_reported = false;
	};
}
