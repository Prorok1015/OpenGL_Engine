#pragma once
#include "res_tag.h"
#include "resources/res_resource_handle.hpp"
#include "desc_base.hpp"
#include <algorithm>
#include <string>
#include <vector>
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

	// Derives the parallel multi-world arrays (names, per-world system lists) from the
	// ordered world list, keeping all three shared_state arrays index-aligned.
	// The world list is the single source of truth for world order (desc order), so deriving
	// names/systems from it avoids the hash-order desync that scn::level::for_each_world
	// (backed by an unordered_map) would otherwise introduce when there are >=2 worlds.
	// Templated on any "world-like" type exposing get_name() and get_systems().
	template<typename World>
	void derive_world_arrays(
		const std::vector<World>& worlds,
		std::vector<std::string>& out_names,
		std::vector<std::vector<std::string>>& out_systems)
	{
		out_names.clear();
		out_systems.clear();
		out_names.reserve(worlds.size());
		out_systems.reserve(worlds.size());
		for (const auto& w : worlds) {
			out_names.emplace_back(w.get_name());
			out_systems.push_back(w.get_systems());
		}
	}

	// World list after a level hot reload: the desc worlds in desc order, then the previous worlds that
	// are not in the desc but still alive in the level (created in the editor, not saved yet).
	// Worlds the reload dropped (is_alive false) are not kept.
	template<typename World, typename Alive>
	std::vector<World> merge_reloaded_worlds(
		const std::vector<World>& desc_worlds,
		const std::vector<World>& previous,
		Alive&& is_alive)
	{
		std::vector<World> merged = desc_worlds;
		for (const auto& p : previous) {
			const bool in_desc = std::any_of(desc_worlds.begin(), desc_worlds.end(),
				[&p](const World& w) { return w.get_name() == p.get_name(); });
			if (!in_desc && is_alive(p)) merged.push_back(p);
		}
		return merged;
	}

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
		// The level manager rebuilt the desc worlds on a hot reload: realign the world arrays with the level.
		void resync_worlds_after_reload();

		void add_desc_to_scene(const res::res_handle<desc::desc_base>& handle, const std::string& key);

		// Stored confirm action (persists across frames for ImGui modal)
		std::function<void()> m_confirm_action;

		// Action deferred by "Save & Continue" on an untitled level; run after a successful Save-As.
		std::function<void()> m_pending_after_save;

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
