#pragma once
#include "edt_input_actions_view.h"
#include <snk/editor/edt_panel_base.h>
#include <snk/game/gs_input_actions.h>
#include <snk/input/inp_actions.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace edt
{
	// "Input Actions" (View menu, closed by default; US-542-C): the actions of the project with their bindings, and the
	// player's changes to them. "Assign" on a binding (or "+" for a new one) waits for the next key or mouse button of this
	// window while it is focused; Esc or leaving the window cancels. A binding another action has is kept and shown as a
	// conflict; Reset puts the project's bindings back. Only draws the rows of `input_actions_view` and calls
	// `gs::input_actions`; the changes are saved there (#736).
	class input_actions_panel : public panel_base
	{
	public:
		using actions_getter = std::function<gs::input_actions*()>;
		using gamepad_poll = std::function<std::optional<inp::action_binding>()>;

		input_actions_panel();

		void set_actions_getter(actions_getter getter) { m_actions = std::move(getter); }
		// The place a gamepad button will come from once the editor has a device source (US-542-B0): asked while waiting.
		void set_gamepad_poll(gamepad_poll poll) { m_gamepad = std::move(poll); }

		// True while the panel waits for a key and was drawn in the last frame or this one: the editor's own shortcuts must stay
		// quiet (modal_active, #470). A frame the panel is not drawn in (the game layout, a hidden tab) does not hold them.
		bool capturing() const;

	protected:
		void on_render() override;
		void on_hidden() override { m_capture.cancel(); }

	private:
		// True when it took a key or a button this frame.
		bool update_capture(gs::input_actions& actions);
		void rebuild_rows(const gs::input_actions& actions);

		actions_getter m_actions;
		gamepad_poll m_gamepad;
		input_actions_view::capture m_capture;
		std::vector<input_actions_view::action_row> m_rows;
		std::uint64_t m_rows_revision = ~std::uint64_t{ 0 };
		std::string m_message;
		int m_rendered_frame = -100;
	};
}
