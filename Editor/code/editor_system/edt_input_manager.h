#pragma once
#include "common.h"
#include "inp_input_actions.h"
#include "inp_input_manager.h"

namespace edt
{
	class input_manager : public inp::input_manager
	{
	public:
		input_manager()
			: inp::input_manager("editor", 0) 
		{
			set_active_layer("editor");
		}

		virtual ~input_manager() override {};

		// TODO: delete legacy
		virtual bool on_handle_event(const inp::input_event&) override;
		virtual void on_notify_listeners(float dt) override;
		//

		virtual bool on_handle_event(wnd::handle, const wnd::input_event&) override;

		void set_input_area(const glm::ivec4& rect, bool invert = false) {
			input_area = rect;
			this->invert = invert;
		}

	private:
		glm::vec2 last_cursor_pos = { 0.f, 0.f };
		glm::ivec4 input_area = {};
		bool invert = false;
	};

	using InputManagerRef = std::shared_ptr<input_manager>;
}
