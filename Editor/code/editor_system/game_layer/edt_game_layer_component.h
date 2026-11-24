#pragma once
#include "edt_module_swap_gate.h"
#include <snk/model_codec/res_tag.h>
#include <cstdint>
#include <functional>

namespace edt
{
	class asset_open_component;
	struct editor_services;
	class level_controller;
	class level_load_component;
	class prefab_editor_component;

	// What the editor does when the game layer changes, a game module loaded, unloaded or reloaded (#435, #1004): nothing of the
	// editor may refer to the old layer once it goes (Play, the prefab preview, the loads in flight, the level), and the level is
	// built again in the new one. It owns the swap gate the builder and Play hand to the host's reload, the level a change
	// interrupted and the generation counter `project.migrate` binds an approved plan to. It listens to the context manager from
	// its constructor, so it is made before what takes the gate (the build component and Play).
	class game_layer_component
	{
	public:
		struct ports
		{
			editor_services& services;
			level_controller& level_ctrl;
			level_load_component& level_load;
			prefab_editor_component& prefab;
			asset_open_component& asset_open;
			// Ends the person's Play (a plain module load or unload: nothing restarts it); late-bound, Play is made after.
			std::function<void()> stop_play = {};
			// The mesh modeller's documents drop their preview worlds before the layer goes and make them again after it (EPIC-77, #1011).
			std::function<void()> meshes_before = {};
			std::function<void()> meshes_after = {};
		};

		explicit game_layer_component(ports from);
		~game_layer_component();
		game_layer_component(const game_layer_component&) = delete;
		game_layer_component& operator=(const game_layer_component&) = delete;

		// Changes of the game layer reach the editor through it; a reload of the game module holds them (#435).
		module_swap_gate& swap_gate() { return m_gate; }
		// Moves with every change of the game layer: project.migrate binds the plan a person approved to the code that runs it.
		std::uint64_t generation() const { return m_generation; }

	private:
		// `before`: the layer is about to go; else it is there again.
		void follow(bool before);

		ports m_ports;
		module_swap_gate m_gate;
		// The level whose load the change interrupted; loaded again into the new layer.
		res::tag m_level_after_change;
		std::uint64_t m_generation = 0;
	};
}
