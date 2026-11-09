#include <snk/game/gs_input_routing.h>
#include <snk/common/logger/engine_log.h>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_input_router.h>
#include <snk/input/inp_input_stack.h>
#include <snk/input/inp_events.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/ecs/ecs_system_registry.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

	inp::input_target target_of(std::string_view set, std::string_view world)
	{
		return { std::string{ set }, std::string{ world } };
	}

	std::vector<inp::KEYBOARD_BUTTONS> keys_of(const scn::world_context& context, const std::vector<std::string>& names)
	{
		static std::set<std::string> reported;
		std::vector<inp::KEYBOARD_BUTTONS> keys;
		for (const std::string& name : names) {
			if (const auto key = inp::key_from_name(name)) {
				keys.push_back(*key);
			} else if (reported.insert(std::string{ context.get_name() } + "/" + name).second) {
				egLOG_WARN("input", "world '{}': input_stack names no key '{}', ignored", context.get_name(), name);
			}
		}
		return keys;
	}

	// The layer every world has: what its UI laid out last frame plus the level's defaults.
	inp::stack_layer_desc base_layer(const entry& e)
	{
		inp::stack_layer_desc layer;
		layer.name = "world";
		layer.order = e.context->input_layer();
		if (e.capture) {
			layer.regions = e.capture->regions;
			layer.whole_keyboard = e.capture->keyboard;
		}
		if (const auto& defaults = e.context->input_stack()) {
			if (defaults->order) layer.order = *defaults->order;
			layer.active = defaults->active;
			layer.block_below = defaults->block_below;
			layer.text_keys = defaults->text_keys;
			layer.keys = keys_of(*e.context, defaults->keys);
			layer.actions = defaults->actions;
			layer.listen = defaults->listen;
		}
		return layer;
	}

	constexpr std::string_view k_update_actions = "inp::update_actions";

	bool lists_update_actions(const scn::world_context& context)
	{
		const auto& systems = context.systems();
		return systems && std::ranges::find(*systems, k_update_actions) != systems->end();
	}

	bool warn_once(std::string key)
	{
		static std::set<std::string> warned;
		return warned.insert(std::move(key)).second;
	}

	// Until the action masks of #624 one world of a set reads the actions: the first bound one in the level file
	// (INPUT_STACK.md M6). The others, and a bound world that would not run inp::update_actions, are warned about once.
	const entry* actions_reader(const std::vector<entry>& entries, std::string_view set, bool have_actions)
	{
		const entry* first = nullptr;
		std::size_t readers = 0;
		for (const auto& e : entries) {
			// A prefab preview shows content, it plays no game.
			if (!e.routed.focused || e.context->kind() == scn::context_kind::preview) continue;
			if (!first || e.context->desc_index() < first->context->desc_index()) first = &e;
			const bool runs = lists_update_actions(*e.context);
			readers += runs ? 1 : 0;
			if (have_actions && !runs && warn_once(std::string{ set } + "/" + std::string{ e.context->get_name() } + "#system")) {
				egLOG_WARN("input", "world '{}' of '{}' gets the input but does not list {}: game actions do nothing in it",
					e.context->get_name(), set, k_update_actions);
			}
		}
		if (have_actions && readers > 1 && first && warn_once(std::string{ set } + "#readers")) {
			egLOG_WARN("input", "set '{}': {} bound worlds run {}; only '{}', the first in the level, gets the actions (until #624)",
				set, readers, k_update_actions, first->context->get_name());
		}
		return first;
	}

	// Mouse and keyboard parts of the full state are given out separately: a context may take the
	// keyboard of a focused text field while the mouse belongs to someone else. The cursor position goes to everyone.
	void write(const entry& e, const inp::world_input* in, const inp::input_bound& bound, std::shared_ptr<const inp::action_map> actions)
	{
		inp::input_state state = e.routed.state;
		const inp::input_state neutral = inp::input_router::neutral(state);
		const bool buttons = in && in->buttons;
		const bool motion = in && in->motion;
		if (!buttons) {
			state.mouse = neutral.mouse;
			state.scroll = neutral.scroll;
		}
		if (!motion) {
			state.mouse_prev = neutral.mouse_prev;
			state.mouse_dir = neutral.mouse_dir;
		}
		for (std::size_t k = 0; k < state.keyboard.size(); ++k) {
			state.keyboard[k] = state.keyboard[k] && in && in->keys[k];
		}
		auto& ctx = e.context->state().ctx();
		ctx.insert_or_assign(state);
		ctx.insert_or_assign(inp::input_focus{ in && (in->mouse_focus || in->keyboard_focus) });
		ctx.insert_or_assign(bound);
		ctx.insert_or_assign(inp::action_map_ref{ std::move(actions) });
	}

	void route_set(inp::input_router& router, scn::context_set& set, const std::shared_ptr<const inp::action_map>& actions)
	{
		const inp::input_state& raw = router.raw_state();
		const glm::vec2 cursor = raw.mouse_pos;
		const bool button_down = any_mouse_button(raw);
		const bool engaged = button_down || raw.scroll != glm::vec2{ 0.f };

		std::vector<entry> entries;
		set.for_each([&](scn::world_context& context) {
			entry e{ &context, router.route(set.name(), context.get_name()), nullptr };
			e.capture = context.state().ctx().find<inp::input_capture>();
			entries.push_back(std::move(e));
		});

		inp::input_stack& stack = router.stack(set.name());
		std::vector<std::string> present;
		std::vector<std::string> fed;
		for (const auto& e : entries) {
			const std::string name{ e.context->get_name() };
			stack.put(name, e.context->desc_index(), base_layer(e));
			present.push_back(name);
			// Contexts the router handed the full state to; the rest stay neutral as before.
			if (e.routed.focused) fed.push_back(name);
		}
		stack.retain_worlds(present);

		const std::string_view set_name = set.name();
		inp::input_router::mouse_hold hold = router.hold(set_name);
		if (!fed.empty() && button_down && !hold.active) {
			// The press (or the first sight of an already pressed button) decides who owns the drag.
			const std::optional<std::string> taker = inp::mouse_taker(stack, fed, cursor);
			hold = { true, taker.has_value(), taker ? target_of(set_name, *taker) : inp::input_target{} };
			router.set_hold(set_name, hold);
		}

		// An owner that is gone (destroyed, or no longer fed by the target) leaves the held
		// button with nobody until it is released: it must not fall through to the others.
		const inp::drag_hold drag{ hold.active, hold.captured, hold.owner.set == set_name ? hold.owner.context : std::string{} };
		const inp::stack_resolution resolved = inp::resolve(stack, fed, cursor, engaged, drag);

		const entry* reader = actions_reader(entries, set_name, actions != nullptr);
		const bool stepping = set.mode() == ecs::run_mode::step;
		for (const auto& e : entries) {
			const inp::input_bound bound{ e.routed.focused, stepping && !e.routed.focused };
			write(e, resolved.find(e.context->get_name()), bound, &e == reader ? actions : std::shared_ptr<const inp::action_map>{});
		}
	}
}

void gs::route_input(inp::input_router& router, scn::context_manager& contexts)
{
	route_input(router, contexts, nullptr);
}

void gs::route_input(inp::input_router& router, scn::context_manager& contexts, const std::shared_ptr<const inp::action_map>& actions)
{
	contexts.for_each_set([&router, &actions](scn::context_set& set) {
		route_set(router, set, actions);
	});
	// Cleared after the pass: on the frame the last button goes up the owner still sees it.
	if (!any_mouse_button(router.raw_state())) {
		router.clear_holds();
	}
}
