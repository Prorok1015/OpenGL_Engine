#pragma once
#include <snk/editor/edt_registration.h>

#include <memory>
#include <vector>

namespace res { class resource_system; }
namespace desc { class desc_system; }
namespace gui { class theme_service; }

namespace edt
{
	class editor_context;
	class editor_layer;
	class editor_settings_panel;
	class theme_settings_model;

	// The editor's theme (#1109): applies the person's choice (edt://settings/editor_theme.json) at startup unless
	// gui/theme names a preset, and the Editor Settings window with its action (Edit > Editor Settings...). Neither
	// is reachable by an agent: the action is not_for_agents and no command writes a preset or the choice.
	class theme_settings_component
	{
	public:
		struct ports
		{
			editor_context& context;
			editor_layer& layer;
			res::resource_system& resources;
			desc::desc_system& descs;
			// Null without a renderer.
			gui::theme_service* themes = nullptr;
		};

		explicit theme_settings_component(ports from);
		~theme_settings_component();
		theme_settings_component(const theme_settings_component&) = delete;
		theme_settings_component& operator=(const theme_settings_component&) = delete;

		void install(std::vector<registration>& out);
		// The Editor Settings window, for the sections other components add to it (Autosave, #1186).
		editor_settings_panel& settings_panel() const;

	private:
		ports m_ports;
		std::unique_ptr<theme_settings_model> m_model;
		std::shared_ptr<editor_settings_panel> m_panel;
	};
}
