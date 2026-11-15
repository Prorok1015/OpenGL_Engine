#pragma once
#include "edt_live_mode.h"
#include "edt_project_trust_service.h"
#include <snk/editor/edt_registration.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace core { class project_module_host; }
namespace prj { struct project_context; }
namespace res { class resource_system; }

namespace edt
{
	class build_settings_panel;
	class editor_context;
	class editor_layer;
	class game_builder;
	class game_reloader;
	class problems_panel;

	// Live mode and the question "Trust this project?" whole: live mode in the editor (its settings, the Build Settings window's
	// switch, the status in the menu bar, its frame), the dialog that grants trust, the load of the modules that waited for it
	// and the live mode that comes on once trust is given. editor_system creates it after the game builder and the reloader it
	// calls, and hands its `ask_trust` to everything that has to ask the person (the Project menu, the first frame of a project,
	// Build, Reload, live mode itself).
	class live_mode_component
	{
	public:
		struct ports
		{
			editor_context& context;
			res::resource_system& resources;
			editor_layer& layer;
			std::weak_ptr<project_trust_service> trust;
			game_builder& builder;
			game_reloader& reloader;
			build_settings_panel& build_settings;
			problems_panel& problems;
			// Null without a project.
			const prj::project_context* project;
			// The host appears in the storage after init (main loads the project's modules then): looked up when used.
			std::function<core::project_module_host*()> module_host;
			// Why the modules cannot load now: a migration runs, a level or an asset loads, Play runs. In this order; null/empty: none.
			std::function<std::optional<std::string>()> migration_blocker;
			std::function<std::optional<std::string>()> load_in_progress;
			std::function<bool()> play_active;
			// Live mode came on or went off: the tools an agent sees change.
			std::function<void()> tools_changed;
		};

		explicit live_mode_component(ports from);
		~live_mode_component();

		// The status slot of the menu bar and the frame stage; nothing is registered anywhere else.
		void install();

		// Asks the person (the dialog of ask_project_trust); `on_trusted` runs once the modules that waited have loaded.
		void ask_trust(std::function<void()> on_trusted);
		bool enabled() const { return m_live && m_live->enabled(); }
		void tick();
		void draw_status() const;

	private:
		ports m_ports;
		trusted_followups m_followups;
		std::unique_ptr<live_mode> m_live;

		// project_module_host::load_waiting on a UI frame where no level load, migration or Play holds it back; `then`
		// runs on the frame after the load.
		void load_waiting_modules(std::function<void()> then);
		// Live mode at open (#908): on when the person turned it on for this project and it may run now.
		void apply_live_consent();
	};
}
