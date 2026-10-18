#pragma once
#include <functional>
#include <entt/fwd.hpp>
#include "edt_panel_base.h"
#include "edt_game_view_logic.h"

namespace rnd { class render_system; }
namespace gui { class gui_system; }

namespace edt
{
	// Shows what a game camera sees. Only the picked camera renders; no gizmos or overlays.
	class game_view_panel : public panel_base
	{
	public:
		game_view_panel(rnd::render_system& rnd, gui::gui_system& gui);

		using registry_visitor = std::function<void(entt::registry&)>;

		void set_registry_getter(std::function<entt::registry*()> getter) { m_get_registry = std::move(getter); }
		// Visits every world's registry: game cameras of inactive worlds must be switched off too.
		void set_all_registries(std::function<void(const registry_visitor&)> for_each) { m_for_each_registry = std::move(for_each); }

	protected:
		void on_render() override;
		// Closed, collapsed or in a hidden tab: stop rendering the picked camera.
		void on_hidden() override;

	private:
		rnd::render_system& m_rnd;
		gui::gui_system&    m_gui;
		void turn_off_cameras_except_in(const entt::registry* keep);

		std::function<entt::registry*()> m_get_registry;
		std::function<void(const registry_visitor&)> m_for_each_registry;
		game_view::selection m_selection;
	};
}
