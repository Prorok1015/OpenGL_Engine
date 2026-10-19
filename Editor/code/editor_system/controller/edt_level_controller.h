#pragma once
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <snk/desc/desc_base.hpp>
#include <snk/scene/level/scn_world_desc.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
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

	// Replaces the editor's world list by merge_reloaded_worlds(desc_worlds, worlds, is_alive) and re-derives the
	// parallel arrays. Returns the index of the world to keep active: the one called `active_name`, else the first,
	// -1 when no world is left (the arrays are then empty too).
	template<typename World, typename Alive>
	int resync_world_arrays(
		const std::vector<World>& desc_worlds,
		std::vector<World>& out_worlds,
		std::vector<std::string>& out_names,
		std::vector<std::vector<std::string>>& out_systems,
		std::string_view active_name,
		Alive&& is_alive)
	{
		out_worlds = merge_reloaded_worlds(desc_worlds, out_worlds, is_alive);
		derive_world_arrays(out_worlds, out_names, out_systems);
		if (out_names.empty()) return -1;
		const auto it = std::find(out_names.begin(), out_names.end(), active_name);
		return it == out_names.end() ? 0 : static_cast<int>(it - out_names.begin());
	}

	// The editor side of a level reload: realigns the world arrays of `state` with the reloaded level and tells the
	// world controller which world is active (none at all, or the one to switch to). `desc_lists_worlds` is whether
	// the level desc names any world; if it does but none of them loaded (`desc_worlds` empty) the state is kept.
	void apply_world_resync(
		shared_state& state,
		world_controller& worlds,
		const std::vector<scn::world_desc>& desc_worlds,
		bool desc_lists_worlds,
		const std::function<bool(const scn::world_desc&)>& is_alive);

	class level_controller
	{
	public:
		level_controller(shared_state& state, res::resource_system& res, desc::desc_system& desc,
			scene_editor& scene_ed, world_controller& world_ctrl);
		~level_controller();

		void new_level();
		// Writes the level to `rel_path` and makes it the level's saved path; false if nothing is loaded or the write failed.
		bool save_level_as(const std::string& rel_path);
		void quick_save_level();

		void finish_level_load(res::res_handle<scn::level_desc>& handle);

		boost::json::object load_desc_template(const std::string& filename);
		boost::json::object build_level_json() const;
		bool write_level_to_disk(const std::string& rel_path);
		void populate_worlds_from_level(const res::res_handle<scn::level_desc>& level_res);
		// The level manager rebuilt the desc worlds on a hot reload: realign the world arrays with the level.
		void resync_worlds_after_reload();

		void add_desc_to_scene(const res::res_handle<desc::desc_base>& handle, const std::string& key);

		// Callbacks set by the coordinator
		std::function<void()> on_editor_world_reloaded;

	private:
		shared_state& m_state;
		res::resource_system& m_res;
		desc::desc_system& m_desc;
		scene_editor& m_scene_editor;
		world_controller& m_world_ctrl;
	};
}
