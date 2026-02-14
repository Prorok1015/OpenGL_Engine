#pragma once
#include "gui_layer_interface.h"
#include "gui_menu_layout.h"

namespace gui {
	class debug_layer : public gui::layer_interface {
	public:
		using UI_CALLBACK = gui::menu_layout_manager::UI_CALLBACK;
		using UI_SWITCH_CALLBACK = gui::menu_layout_manager::UI_SWITCH_CALLBACK;

		debug_layer() {
			m_manager.set_show_main_menu(true);

			m_manager.registrate("Debug/Imgui demo window", [this] { return show_demo(); });
			m_manager.registrate("Debug/Statistic", [this] {  return show_stats(); });
			m_manager.set_menu_checked("Degub/Statistic", true);

		}

		virtual void on_update(std::chrono::duration<float> dt) override;

		void register_tool(const std::string_view path, UI_CALLBACK callback) {
			m_manager.registrate(path, callback);
		}

		void unregister_tool(const std::string_view path) {
			m_manager.unregistrate(path);
		}

		void register_implicit(const std::string_view id, UI_CALLBACK callback) {
			m_manager.register_implicit(id, callback);
		}

		void unregister_implicit(const std::string_view id) {
			m_manager.unregister_implicit(id);
		}

		void set_tool_checked(const std::string_view path, bool checked) {
			m_manager.set_menu_checked(path, checked);
		}

		bool is_tool_checked(const std::string_view path) const {
			return m_manager.get_is_item_checked(path);
		}

		void set_tool_check_callback(const std::string_view path, UI_SWITCH_CALLBACK callback) {
			m_manager.set_check_callback(path, callback);
		}

		bool show_stats();
		bool show_demo();
	private:
		gui::menu_layout_manager m_manager;
	};
}