#pragma once
#include "edt_registration.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	class panel_base;

	// Where a panel is docked the first time the editor opens (and after the layout is reset).
	// `floating` panels are not docked.
	enum class dock_slot { floating, left, right, center, bottom_left, bottom_right };

	// Name of the menu the panel toggles live in; the main menu draws them after the actions of that menu.
	inline constexpr std::string_view view_menu_name = "View";

	struct panel_desc
	{
		// Must equal panel_base::get_id() / get_title(): the title is the ImGui window name the dock layout targets.
		std::string id;
		std::string title;
		dock_slot slot = dock_slot::floating;
		bool open_by_default = true;
		// A "View" menu entry that toggles the panel.
		bool show_in_view_menu = true;
		std::shared_ptr<panel_base> panel;
	};

	struct panel_info
	{
		std::string id;
		std::string title;
		dock_slot slot = dock_slot::floating;
		bool open = false;
		bool show_in_view_menu = true;
	};

	// Panels of the editor in registration order. That order is the render order, the order of the View menu
	// and of the tabs docked into one slot. Built-in panels go through the same add() as extensions. Main thread only.
	class panel_registry
	{
	public:
		panel_registry();
		~panel_registry();
		panel_registry(const panel_registry&) = delete;
		panel_registry& operator=(const panel_registry&) = delete;

		// Sets the panel open or closed to `open_by_default`. An empty id, a taken id, a null panel or an
		// id/title that differs from the panel's own is an error: it is logged and nothing is registered
		// (the returned registration is not active).
		[[nodiscard]] registration add(panel_desc desc);

		bool contains(std::string_view id) const;
		panel_base* find(std::string_view id) const;
		std::size_t size() const { return m_entries.size(); }

		bool is_open(std::string_view id) const;
		// False if there is no such panel.
		bool set_open(std::string_view id, bool open);

		// Snapshot in registration order.
		std::vector<panel_info> panels() const;

		void render_all();

	private:
		struct entry;
		void remove(std::uint64_t key);

		std::vector<std::shared_ptr<entry>> m_entries;
		std::uint64_t m_next_key = 1;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
