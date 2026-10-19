#pragma once
#include <functional>
#include <optional>
#include <snk/resource/res_tag.h>
#include <entt/fwd.hpp>
#include "edt_panel_base.h"
#include "edt_game_view_logic.h"
#include "edt_input_binding.h"

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
		// Visits every world's registry: the overlay worlds of a set draw into the picked camera's texture, and the
		// game cameras of everything else must be switched off.
		void set_all_registries(std::function<void(const registry_visitor&)> for_each) { m_for_each_registry = std::move(for_each); }

		// The image feeds the context `target` names (a play context; none in Edit mode) while hovered.
		void set_input_router(inp::input_router* router, input_target_getter target) {
			m_router = router;
			m_input_target = std::move(target);
		}

		// The render target of the picked camera while the panel shows it; none when it is hidden or has none.
		// The "game" view of viewport.capture.
		const std::optional<res::tag>& render_target() const { return m_target; }

	protected:
		void on_render() override;
		// Closed, collapsed or in a hidden tab: stop rendering the picked camera.
		void on_hidden() override;

	private:
		std::optional<res::tag> m_target;
		rnd::render_system& m_rnd;
		gui::gui_system&    m_gui;
		// Gives the game cameras that draw into `target` (in the registry of any world) `size`, switches the others off.
		void apply_viewports_to_all(const res::tag& target, glm::ivec2 size);

		std::function<entt::registry*()> m_get_registry;
		std::function<void(const registry_visitor&)> m_for_each_registry;
		game_view::selection m_selection;
		inp::input_router* m_router = nullptr;
		input_target_getter m_input_target;
	};
}
