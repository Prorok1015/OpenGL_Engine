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

		void set_registry_getter(std::function<entt::registry*()> getter) { m_get_registry = std::move(getter); }

	protected:
		void on_render() override;
		// Closed, collapsed or in a hidden tab: stop rendering the picked camera.
		void on_hidden() override;

	private:
		rnd::render_system& m_rnd;
		gui::gui_system&    m_gui;
		std::function<entt::registry*()> m_get_registry;
		game_view::selection m_selection;
	};
}
