#pragma once
#include <functional>
#include <memory>
#include <string>

namespace prj { struct held_project; }

namespace edt
{
	class editor_context;
	class editor_layer;
	class editor_mode_controller;
	class level_load_component;
	class project_trust_service;

	// What the editor does on its first frames (#1004): asks about a project held back for its engine version, loads the start
	// level (--game, editor/startup_level or the project's own) on the first frame and, for game mode, hands the loading screen to
	// the mode controller, and asks the person about the project's modules once main has loaded them (#921). It registers
	// implicit callbacks on the layer and owns nothing else; the two install functions are called where the old init() had the
	// code, because the order of implicit callbacks is the order they are registered in.
	class startup_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_layer& layer;
			level_load_component& level_load;
			// The project held back for its engine version, if any: nothing of it is mounted and no start level is loaded.
			prj::held_project* held = nullptr;
			std::weak_ptr<project_trust_service> trust = {};
			bool headless = false;
			// Late-bound: the mode controller and live mode are made before the first frame but after the level load.
			std::function<std::string(const std::string& configured)> start_level_name = {};
			std::function<editor_mode_controller*()> modes = {};
			std::function<void(std::function<void()> on_trusted)> ask_trust = {};
			std::function<void()> exit = {};
			// Set with editor/exit_after_level_loaded; empty otherwise.
			std::function<void()> stop_after_level_loaded = {};
		};

		explicit startup_component(ports from);
		~startup_component();
		startup_component(const startup_component&) = delete;
		startup_component& operator=(const startup_component&) = delete;

		// The question about a held project and the load of the start level.
		void install_start_level();
		// The question about the project's modules (nothing is asked while a project is held back).
		void install_trust_check();

	private:
		ports m_ports;
	};
}
