#pragma once
#include "gui_renderer.h"
#include "gui_menu_layout.h"

namespace gui
{
	class gui_system
	{
	public:
		gui_system();
		~gui_system();

		gui_system(gui_system&&) = delete;
		gui_system& operator= (gui_system&&) = delete;

		gui_system(const gui_system&) = delete;
		gui_system& operator= (const gui_system&) = delete;

		void render_menues();

		void set_show_title_bar(bool show);
		void set_show_title_bar_dbg(bool show);

		void set_show_ui(bool show) { is_show = show; }
		bool is_hiden() const { return !is_show; }

		//void switch_input(inp::KEYBOARD_BUTTONS code, inp::KEY_ACTION action);
		void set_is_input_enabled(bool enable);

		void registrate_menu(std::string_view path, menu_layout_manager::UI_CALLBACK callback);
		void registrate_menu_dbg(std::string_view path, menu_layout_manager::UI_CALLBACK callback);
		void unregistrate_menu(std::string_view path);
		void unregistrate_menu_dbg(std::string_view path);

		void register_implicit(const std::string_view id, menu_layout_manager::UI_CALLBACK callback);
		void register_implicit_dbg(const std::string_view id, menu_layout_manager::UI_CALLBACK callback);
		void unregister_implicit(const std::string_view id);
		void unregister_implicit_dbg(const std::string_view id);

		void set_menu_checked(const std::string_view path, bool checked);
		void set_menu_checked_dbg(const std::string_view path, bool checked);
		bool is_item_checked(const std::string_view path) const;
		bool is_item_checked_dbg(const std::string_view path) const;
		void set_check_callback(const std::string_view path, menu_layout_manager::UI_SWITCH_CALLBACK callback);
		void set_check_callback_dbg(const std::string_view path, menu_layout_manager::UI_SWITCH_CALLBACK callback);

		bool show_stats();
		bool show_demo();
	private:
		bool is_show = true;
		bool is_input_enabled = true;
		std::shared_ptr<renderer> renderer;

		menu_layout_manager layout;
		menu_layout_manager dbg_layout;
	};

	gui_system& get_system();
}