#pragma once
#include "edt_panel_base.h"
#include <memory>
#include <vector>
#include <string_view>

namespace edt
{
	class panel_manager
	{
	public:
		void add_panel(std::shared_ptr<panel_base> panel);
		void remove_panel(std::string_view id);
		panel_base* find_panel(std::string_view id) const;
		void render_all();
		void render_view_menu();

	private:
		std::vector<std::shared_ptr<panel_base>> m_panels;
	};
}
