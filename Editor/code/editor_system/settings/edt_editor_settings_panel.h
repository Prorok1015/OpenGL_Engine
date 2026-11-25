#pragma once
#include "../autosave/edt_autosave_settings.h"
#include "../modeller/edt_modeller_settings.h"
#include <snk/editor/edt_panel_base.h>

#include <array>
#include <functional>
#include <string>

namespace edt
{
	class theme_settings_model;

	// What the window's Autosave section reads and changes (#1186); without `get` and `set` the section is not shown.
	struct autosave_settings_port
	{
		std::function<autosave_settings()> get;
		// Applies the settings and stores them in the person's settings; the error, empty when they were written.
		std::function<std::string(const autosave_settings&)> set;
		// Why autosave writes nothing whatever the settings say (cfg editor/autosave, no project); empty: nothing.
		std::function<std::string()> blocked;
	};

	// What the window's Modeller section reads and changes (#1013): the snap steps of the mesh gizmo.
	struct modeller_settings_port
	{
		std::function<modeller_settings()> get;
		// Applies the settings and stores them in the person's settings; the error, empty when they were written.
		std::function<std::string(const modeller_settings&)> set;
	};

	// "Editor Settings" (#1109): the theme, chosen from the shipped presets and the person's own, and the editor of a
	// preset of edt://themes/ (colors, sizes, font), shown at once and written by Save. Only draws theme_settings_model.
	class editor_settings_panel final : public panel_base
	{
	public:
		static constexpr const char* k_id = "editor_settings";
		static constexpr const char* k_title = "Editor Settings";

		explicit editor_settings_panel(theme_settings_model& model);

		void focus() { m_focus = true; }
		void set_autosave(autosave_settings_port port) { m_autosave = std::move(port); }
		void set_modeller(modeller_settings_port port) { m_modeller = std::move(port); }

	protected:
		void on_before_begin() override;
		void on_render() override;

	private:
		void draw_theme_list();
		void draw_preset_editor();
		void draw_duplicate();
		void draw_autosave();
		void draw_modeller();

		theme_settings_model& m_model;
		std::array<char, 65> m_new_name{};
		bool m_focus = false;
		autosave_settings_port m_autosave;
		modeller_settings_port m_modeller;
		std::string m_modeller_message;
		std::string m_autosave_message;
		// The interval being typed: taken when the field is left.
		int m_interval_edit = 0;
		bool m_interval_active = false;
	};
}
