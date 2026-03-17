#pragma once
#include "edt_panel_manager.h"
#include <imgui.h>
#include <functional>

namespace edt
{
	class dockspace
	{
	public:
		using action = std::function<void()>;

		void set_on_open_level(action cb)  { m_on_open_level  = std::move(cb); }
		void set_on_new_level(action cb)   { m_on_new_level   = std::move(cb); }
		void set_on_save_level(action cb)  { m_on_save_level  = std::move(cb); }
		void set_on_quick_save(action cb)  { m_on_quick_save  = std::move(cb); }
		void set_on_exit(action cb)        { m_on_exit        = std::move(cb); }
		void set_on_import_model(action cb){ m_on_import_model = std::move(cb); }

		void render(panel_manager& pm);

	private:
		void render_menu_bar(panel_manager& pm);
		void setup_default_layout(ImGuiID dockspace_id);

		bool m_layout_initialized = false;

		action m_on_open_level;
		action m_on_new_level;
		action m_on_save_level;
		action m_on_quick_save;
		action m_on_exit;
		action m_on_import_model;
	};
}
