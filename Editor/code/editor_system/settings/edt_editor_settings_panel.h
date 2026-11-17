#pragma once
#include <snk/editor/edt_panel_base.h>

#include <array>

namespace edt
{
	class theme_settings_model;

	// "Editor Settings" (#1109): the theme, chosen from the shipped presets and the person's own, and the editor of a
	// preset of edt://themes/ (colors, sizes, font), shown at once and written by Save. Only draws theme_settings_model.
	class editor_settings_panel final : public panel_base
	{
	public:
		static constexpr const char* k_id = "editor_settings";
		static constexpr const char* k_title = "Editor Settings";

		explicit editor_settings_panel(theme_settings_model& model);

		void focus() { m_focus = true; }

	protected:
		void on_before_begin() override;
		void on_render() override;

	private:
		void draw_theme_list();
		void draw_preset_editor();
		void draw_duplicate();

		theme_settings_model& m_model;
		std::array<char, 65> m_new_name{};
		bool m_focus = false;
	};
}
