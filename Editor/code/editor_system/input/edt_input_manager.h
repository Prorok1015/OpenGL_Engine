#pragma once
#include "common.h"
#include "inp_input_manager_base.h"

namespace edt
{
	// ImGui reports rects in window units; cursor events arrive in framebuffer pixels. This is the
	// single place where the two meet: pass ImGui::GetIO().DisplayFramebufferScale as fb_scale.
	inline glm::ivec4 make_input_area(glm::vec2 win_pos, glm::vec2 size, glm::vec2 fb_scale)
	{
		const glm::vec2 min = win_pos * fb_scale;
		const glm::vec2 max = (win_pos + size) * fb_scale;
		return glm::ivec4{ static_cast<int>(min.x), static_cast<int>(min.y), static_cast<int>(max.x), static_cast<int>(max.y) };
	}

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
