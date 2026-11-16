#pragma once
#include "edt_game_builder.h"
#include "edt_game_reloader.h"
#include "edt_project_trust_service.h"
#include <snk/editor/edt_registration.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace core { class project_module_host; }
namespace prj { struct project_context; }

namespace edt
{
	class build_settings_panel;
	class editor_context;
	class editor_layer;
	class module_swap_gate;
	class play_session;

	// Building the game module and swapping it in, whole: the builder (Build > Build Game), the reloader (Build > Reload Game Module,
	// the reload after a build, the one live mode asks for), what both ask of the rest of the editor, and the commands of Build
	// Settings. editor_system creates it before live mode and the trust dialog, which take its builder and reloader, and calls
	// `install()` where the Build Settings entries go in.
	class game_build_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_layer& layer;
			build_settings_panel& settings_panel;
			// Holds the level detached around the swap; the host gets its hooks.
			module_swap_gate& swap_gate;
			std::weak_ptr<project_trust_service> trust;
			// Null without a project.
			const prj::project_context* project;
			// "debuggame" or "release": what a build without an argument builds.
			std::string default_configuration;
			// The host appears in the storage after init (main loads the project's modules then): looked up when used.
			std::function<core::project_module_host*()> module_host;
			// Asks the person about trust and runs the callback once it is given.
			std::function<void(std::function<void()>)> ask_trust;
			// Why a build or a reload must wait: a migration runs, a level or an asset loads. Null/empty: none.
			std::function<std::optional<std::string>()> migration_blocker;
			std::function<std::optional<std::string>()> load_in_progress;
			std::function<bool()> level_switch_pending;
			std::function<bool()> prefab_has_unapplied_edits;
			std::function<bool()> live_mode_enabled;
			// Null before the Play component exists and after it is gone.
			std::function<play_session*()> play;
		};

		explicit game_build_component(ports from);
		~game_build_component();
		game_build_component(const game_build_component&) = delete;
		game_build_component& operator=(const game_build_component&) = delete;

		// Build > Build Settings... and the build.settings.* commands.
		void install(std::vector<registration>& out);

		game_builder& builder() { return *m_builder; }
		game_reloader& reloader() { return *m_reloader; }

	private:
		ports m_ports;
		std::unique_ptr<game_builder> m_builder;
		// After the builder whose state it asks, so it is destroyed first.
		std::unique_ptr<game_reloader> m_reloader;
	};
}
