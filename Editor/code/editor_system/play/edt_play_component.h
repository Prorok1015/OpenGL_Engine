#pragma once
#include "edt_editor_mode.h"
#include "edt_play_session.h"
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/command/cmd_policy.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_registration.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core { class project_module_host; }
namespace inp { class input_router; }
namespace prj { struct project_context; }
namespace res { class resource_system; }

namespace edt
{
	class editor_context;
	class editor_layer;
	class game_view_panel;
	class inspector_panel;
	class module_swap_gate;
	class play_preflight_publisher;
	class problems_panel;
	class scene_hierarchy_panel;
	class scene_document;
	class viewport_panel;
	class world_controller;
	struct editor_services;

	// Play and the editor/game mode, whole: the session of the scene document, the locks it puts on the panels, the Play menu
	// and the agent's play.* / runtime.* / world.events commands, the mode controller with F11 and its full-window layout, and the
	// "Play: playing" slot of the menu bar. editor_system creates it, registers it once and asks it for the session and the mode.
	class play_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_services& services;
			scene_document& level;
			world_controller& worlds;
			res::resource_system& resources;
			inp::input_router* input_router;
			editor_layer& layer;
			module_swap_gate& swap_gate;
			scene_hierarchy_panel& hierarchy;
			inspector_panel& inspector;
			viewport_panel& viewport;
			game_view_panel& game_view;
			problems_panel& problems;
			play_preflight_publisher& preflight;
			// Null without a project.
			const prj::project_context* project;
			// The host appears in the storage after init (main loads the project's modules then): it is looked up when used.
			std::function<core::project_module_host*()> module_host;
			// Null where the application has no frame loop to step (headless tools).
			app::frame_command_registrations* frame_commands;
			app::frame_control* frame_control;
			bool report_full_snapshot;
			// Why Play cannot start besides its own reasons (a migration, a load that is about to change the document).
			std::function<std::optional<std::string>()> start_blocker;
			std::function<bool()> person_is_editing;
			std::function<cmd::policy_mode(std::string_view)> agent_mode_of;
			// Waits for the saves queued to user:// (res::save_service::flush); empty without the service.
			std::function<void()> finish_user_writes;
		};

		explicit play_component(ports from);
		~play_component();

		// The Play menu, the commands and the game layout of the editor layer.
		void install(std::vector<registration>& out);
		// Stops what the destructor of editor_system must stop while the context is still there: `context_alive` false (the
		// storage was torn down without editor_term) abandons Play instead of stopping it.
		void shutdown(bool context_alive);
		// "Play: playing" / "Play: paused" in the menu bar; nothing while stopped.
		void draw_status() const;

		play_session* play() const { return m_play.get(); }
		editor_mode_controller* modes() const { return m_modes.get(); }

	private:
		ports m_ports;
		// Stopped in shutdown() while the context it reports to is still there. Its commands are in the registrations of
		// editor_system, dropped before this component goes.
		std::unique_ptr<play_session> m_play;
		// Releases app.advance's time hold when Play stops (register_play_frame_commands).
		connection m_play_hold_release;
		// Editor or game layout (#294); reset in shutdown() before the Play session it pauses and starts.
		std::unique_ptr<editor_mode_controller> m_modes;
		// Gives the Scene view's camera its size back when the editor layout returns.
		connection m_mode_changed;
	};
}
