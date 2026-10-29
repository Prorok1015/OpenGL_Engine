#pragma once
#include "edt_panel_base.h"
#include "edt_build_settings_model.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	// "Build Settings" (#444, Build > Build Settings..., action build.settings.open): the presets of the game module
	// (team defines read-only, the user's defines and jobs in CMakeUserPresets.json) and .snk/build.json. The state and
	// every file access are build_settings_model; this class only draws it. The files are read when the window opens.
	class build_settings_panel : public panel_base
	{
	public:
		static constexpr const char* k_id = "build_settings";
		static constexpr const char* k_title = "Build Settings";

		// `project_root` nullopt: no project is open. `building`: whether the person's game build runs (build_settings_model::set_building).
		build_settings_panel(std::function<std::optional<std::filesystem::path>()> project_root, std::string default_configuration,
			std::function<bool()> building);

		// Brings the window to the front on its next frame.
		void focus() { m_focus = true; }
		// See build_settings_model::set_on_settings_applied.
		void set_on_settings_applied(std::function<void(const build_settings&)> applied)
		{
			m_model.set_on_settings_applied(std::move(applied));
		}
		// build.json was written elsewhere (Build > Live Mode): read the files again unless edits are queued.
		void refresh_if_clean();
		// See build_settings_model::set_stop_background.
		void set_stop_background(std::function<void()> cancel) { m_model.set_stop_background(std::move(cancel)); }
		// Why live mode cannot be turned on now (empty: it can); null: it always can.
		void set_live_unavailable_reason(std::function<std::string()> reason) { m_live_unavailable = std::move(reason); }

	protected:
		void on_before_begin() override;
		void on_render() override;
		void on_hidden() override;

	private:
		void reload();
		void draw_presets();
		void draw_defines();
		void draw_module();
		void draw_live();
		void draw_footer();
		std::string relative(const std::filesystem::path& file) const;

		std::function<std::optional<std::filesystem::path>()> m_project_root;
		std::function<std::string()> m_live_unavailable;
		build_settings_model m_model;
		bool m_estimated = false;
		bool m_reload_on_open = true;
		bool m_focus = false;

		// The configuration whose defines are shown.
		std::string m_define_configuration;
		// Text of the user define values being edited inline, by name.
		std::map<std::string, std::string> m_value_edits;
		std::string m_new_name;
		std::string m_new_value;
		bool m_add_everywhere = false;
		std::string m_define_error;
		std::vector<std::string> m_apply_errors;

		std::vector<std::filesystem::path> m_changed;
		std::chrono::steady_clock::time_point m_changed_checked{};
	};
}
