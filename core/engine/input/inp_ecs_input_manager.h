#pragma once
#include "common.h"
#include "ecs_system_interface.hpp"
#include "inp_input_manager_base.h"
#include "inp_input_event.hpp"
#include "inp_events.hpp"

namespace inp
{
	class ecs_input_manager : public inp::input_manager_base, public ecs::system_interface
	{
	public:
		ecs_input_manager() : inp::input_manager_base("ecs") {}

		virtual bool on_handle_event(wnd::handle win, const inp::input_event& evt) override;

		void set_input_area(const glm::ivec4& rect, bool invert = false) {
			input_area = rect;
			this->invert = invert;
		}

		void update_input_state_system(inp::input_state& state) {
			state = this->input_state;
		}

		void reset_frame_deltas() {
			input_state.scroll = glm::vec2{};
			input_state.mouse_dir = glm::vec2{};
		}
	private:
		inp::input_state input_state;
		glm::ivec4 input_area = {};
		bool invert = false;
	};
}