#pragma once
#include "common.h"
#include "inp_input_manager_base.h"
#include "ecs_common_system.h"
#include "ecs_entity.h"
#include "inp_input_event.hpp"

namespace ecs
{
	class flow_input_manager : public inp::input_manager_base
	{
	public:
		flow_input_manager()
			: inp::input_manager_base("ecs") {}

		ecs::entity get_empty_entity() {
			if (!ecs_registry->valid(input_event)) {
				input_event = ecs_registry->create();
			}
			return input_event;
		}

		virtual bool on_handle_event(wnd::handle win, const inp::input_event& evt) override;

		void set_input_area(const glm::ivec4& rect, bool invert = false) {
			input_area = rect;
			this->invert = invert;
		}

		entt::registry& get_registry() {
			return *ecs_registry;
		}

		void set_active_registry(const std::shared_ptr<entt::registry>& registry) {
			ecs_registry = registry;
		}
	private:
		std::shared_ptr<entt::registry> ecs_registry;
		ecs::entity input_event = entt::null;
		glm::vec2 last_cursor_pos = { 0.f, 0.f };
		glm::ivec4 input_area = {};
		bool invert = false;
	};
}