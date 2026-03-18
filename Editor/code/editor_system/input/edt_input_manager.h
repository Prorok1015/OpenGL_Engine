#pragma once
#include "common.h"
#include "inp_input_manager_base.h"

namespace edt
{
	class input_manager : public inp::input_manager_base
	{
	public:
		input_manager()
			: inp::input_manager_base("editor") 
		{
		}

		virtual ~input_manager() override = default;

		virtual bool on_handle_event(wnd::handle, const inp::input_event&) override;

		void set_input_area(const glm::ivec4& rect, bool invert = false) {
			input_area = rect;
			this->invert = invert;
		}

	private:
		glm::vec2 last_cursor_pos = { 0.f, 0.f };
		glm::ivec4 input_area = {};
		bool invert = false;
	};
}
