#pragma once
#include "res_tag.h"
#include "resources/res_resource_handle.hpp"
#include "desc_base.hpp"
#include <string>
#include <functional>
#include <boost/json/object.hpp>

namespace res { class resource_system; }
namespace desc { class desc_system; }
namespace scn { class level_desc; }

namespace edt
{
	struct shared_state;
	class scene_editor;
	class world_controller;

	class level_controller
	{
	public:
		level_controller(shared_state& state, res::resource_system& res, desc::desc_system& desc,
			scene_editor& scene_ed, world_controller& world_ctrl);
		~level_controller();

		void new_level();
		bool save_level();
		void quick_save_level();
		bool show_exit_confirm();

		void finish_level_load(res::res_handle<scn::level_desc>& handle);

		boost::json::object load_desc_template(const std::string& filename);
		boost::json::object build_level_json() const;
		bool write_level_to_disk(const std::string& rel_path);
		void populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res);

		void add_desc_to_scene(const res::res_handle<desc::desc_base>& handle, const std::string& key);

		// Stored confirm action (persists across frames for ImGui modal)
		std::function<void()> m_confirm_action;

		// Callbacks set by the coordinator
		std::function<void()> on_editor_world_reloaded;
		std::function<void()> on_need_save_dialog;

		char m_save_path_buf[512] = {};
		bool m_save_dialog_open = false;

	private:
		shared_state& m_state;
		res::resource_system& m_res;
		desc::desc_system& m_desc;
		scene_editor& m_scene_editor;
		world_controller& m_world_ctrl;
	};
}
