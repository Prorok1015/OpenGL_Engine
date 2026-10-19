#include <snk/game/gs_input_routing.h>
#include <snk/input/inp_input_router.h>
#include <snk/input/inp_events.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_context.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	// A context the router fed, with what the layer rules need to know about it.
	struct entry
	{
		scn::world_context* context = nullptr;
		inp::routed_input routed;
		const inp::input_capture* capture = nullptr;
	};

	bool any_mouse_button(const inp::input_state& state)
	{
		return std::ranges::any_of(state.mouse, [](bool down) { return down; });
	}

	bool hits(const entry& e, glm::vec2 pos)
	{
		if (!e.capture) return false;
		return std::ranges::any_of(e.capture->regions, [pos](const glm::ivec4& r) {
			return pos.x >= r.x && pos.x <= r.z && pos.y >= r.y && pos.y <= r.w;
		});
	}

	inp::input_target target_of(std::string_view set, const entry& e)
	{
		return { std::string{ set }, std::string{ e.context->get_name() } };
	}

	// Mouse and keyboard parts of the full state are given out separately: a context may take the
	// keyboard of a focused text field while the mouse belongs to someone else.
	void write(const entry& e, bool mouse, bool keyboard, bool focused)
	{
		inp::input_state state = e.routed.state;
		const inp::input_state neutral = inp::input_router::neutral(state);
		if (!mouse) {
			state.mouse = neutral.mouse;
			state.mouse_prev = neutral.mouse_prev;
			state.mouse_dir = neutral.mouse_dir;
			state.scroll = neutral.scroll;
		}
		if (!keyboard) {
			state.keyboard = neutral.keyboard;
		}
		auto& ctx = e.context->state().ctx();
		ctx.insert_or_assign(state);
		ctx.insert_or_assign(inp::input_focus{ focused });
	}

	void route_set(inp::input_router& router, scn::context_set& set)
	{
		const glm::vec2 cursor = router.raw_state().mouse_pos;
		const bool button_down = any_mouse_button(router.raw_state());

		std::vector<entry> entries;
		set.for_each([&](scn::world_context& context) {
			entry e{ &context, router.route(set.name(), context.get_name()), nullptr };
			e.capture = context.state().ctx().find<inp::input_capture>();
			entries.push_back(std::move(e));
		});

		// Contexts the router handed the full state to; the rest stay neutral as before.
		std::vector<const entry*> group;
		for (const auto& e : entries) {
			if (e.routed.focused) group.push_back(&e);
		}
		// Top layer first; equal layers keep the id order.
		std::stable_sort(group.begin(), group.end(), [](const entry* a, const entry* b) {
			return a->context->input_layer() > b->context->input_layer();
		});

		const auto hit_it = std::ranges::find_if(group, [cursor](const entry* e) { return hits(*e, cursor); });
		const entry* hit = hit_it != group.end() ? *hit_it : nullptr;

		if (!group.empty() && button_down && !router.hold().active) {
			// The press (or the first sight of an already pressed button) decides who owns the drag.
			router.set_hold({ true, hit != nullptr, hit ? target_of(set.name(), *hit) : inp::input_target{} });
		}

		const entry* holder = nullptr;
		if (router.hold().active && router.hold().captured) {
			const auto it = std::ranges::find_if(group, [&](const entry* e) { return target_of(set.name(), *e) == router.hold().owner; });
			if (it != group.end()) holder = *it;
		}
		const bool drag_from_outside = router.hold().active && !router.hold().captured;

		const entry* keyboard_owner = nullptr;
		for (const entry* e : group) {
			if (e->capture && e->capture->keyboard) {
				keyboard_owner = e;
				break;
			}
		}

		for (const auto& e : entries) {
			if (!e.routed.focused) {
				write(e, false, false, false);
				continue;
			}
			bool mouse = true;
			if (holder) {
				mouse = &e == holder;
			} else if (drag_from_outside) {
				mouse = &e != hit;
			} else if (hit) {
				mouse = &e == hit;
			}
			const bool keyboard = keyboard_owner ? &e == keyboard_owner : true;
			// input_focus gates the mouse-driven camera: it follows the mouse, or an explicit keyboard owner
			write(e, mouse, keyboard, mouse || (keyboard_owner && keyboard));
		}
	}
}

void gs::route_input(inp::input_router& router, scn::context_manager& contexts)
{
	if (!any_mouse_button(router.raw_state())) {
		router.clear_hold();
	}
	contexts.for_each_set([&router](scn::context_set& set) {
		route_set(router, set);
	});
}
