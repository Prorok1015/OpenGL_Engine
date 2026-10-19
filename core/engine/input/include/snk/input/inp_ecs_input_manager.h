#pragma once
#include <snk/common/common.h>
#include <snk/ecs/ecs_system_interface.hpp>
#include <snk/input/inp_input_manager_base.h>
#include <snk/core_input/inp_input_event.hpp>
#include <snk/input/inp_events.hpp>
#include <snk/common/snk_api.h>
#include <memory>

namespace ecs { class system_factory; }

namespace inp
{
	// The only receiver of window events for the ECS. Keeps the raw state of the devices and knows
	// nothing about contexts or panels: inp::input_router decides which context sees what.
	class SNK_API ecs_input_manager : public inp::input_manager_base, public ecs::system_interface
	{
	public:
		ecs_input_manager() : inp::input_manager_base("ecs") {}

		virtual bool on_handle_event(wnd::handle win, const inp::input_event& evt) override;

		// Raw state. `mouse_dir` is the last cursor step in pixels (previous - current); the router
		// scales it per binding.
		const inp::input_state& raw() const { return m_raw; }

		// The router writes `inp::input_state` into every context before the update stage; the
		// system exists because descs reference it by name and only keeps the state in the ctx.
		void update_input_state_system(inp::input_state&) {}

		void reset_frame_deltas() {
			m_raw.scroll = glm::vec2{};
			m_raw.mouse_dir = glm::vec2{};
		}
	private:
		inp::input_state m_raw;
	};

	// Registers "inp::update_input_state" (phase `input`) for the given manager instance.
	SNK_API void register_ecs_input_system(ecs::system_factory& factory, std::shared_ptr<ecs_input_manager> manager);
}
