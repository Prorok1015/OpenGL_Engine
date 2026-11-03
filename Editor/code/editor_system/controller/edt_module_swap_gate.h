#pragma once
#include <snk/core/core_project_modules.h>
#include <functional>
#include <utility>

namespace edt {

	// Which changes of the game layer the editor follows while the game module is reloaded (#435). A swap changes the
	// layer twice (module -> empty -> new module, or back to the old one): the level is detached at the first change and
	// attached again only at the end, to whichever layer is the game layer then, never built in the empty one between.
	// editor_system routes context_manager's game layer changes through on_game_layer_change and hands hooks() to
	// core::project_module_host::reload.
	class module_swap_gate
	{
	public:
		// `follow(before)`: the editor's own handling of a change of the game layer (detach before, attach after).
		explicit module_swap_gate(std::function<void(bool before)> follow) : m_follow(std::move(follow)) {}

		// Play around a swap (play_session::before_module_swap / after_module_swap): `before` runs first, while the old
		// layer and its set "play" are still there; `after` runs last, once the level is attached to the new layer.
		void set_play_hooks(std::function<void()> before, std::function<void()> after)
		{
			m_before_swap = std::move(before);
			m_after_swap = std::move(after);
		}

		void on_game_layer_change(bool before)
		{
			if (pass(before)) {
				m_follow(before);
			}
		}

		// Valid while the gate lives.
		core::reload_hooks hooks()
		{
			core::reload_hooks hooks;
			hooks.before_swap = [this] {
				// destroy_layer would take the set "play" with it: Play stops first and starts again after the swap.
				if (m_before_swap) m_before_swap();
				m_swapping = true;
			};
			hooks.after_swap = [this](const core::reload_report&) {
				m_swapping = false;
				if (std::exchange(m_detached, false)) {
					m_follow(false);
				}
				if (m_after_swap) m_after_swap();
			};
			return hooks;
		}

	private:
		bool pass(bool before)
		{
			if (!m_swapping) {
				return true;
			}
			if (!before || m_detached) {
				return false;
			}
			m_detached = true;
			return true;
		}

		std::function<void(bool before)> m_follow;
		std::function<void()> m_before_swap;
		std::function<void()> m_after_swap;
		bool m_swapping = false;
		bool m_detached = false;
	};

}
