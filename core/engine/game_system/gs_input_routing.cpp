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

	// The layer every world has: what its UI laid out last frame plus the level's defaults. `declare_all`: it declares every
	// action of the map when the level lists none (the default reader, INPUT_STACK.md 6).
	inp::stack_layer_desc base_layer(const entry& e, bool declare_all)
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
		layer.all_actions = declare_all && layer.actions.empty();
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

	// A world declares its actions itself when its level lists them (`input_stack.actions`); the others get the default.
	bool declares_actions(const scn::world_context& context)
	{
		const auto& defaults = context.input_stack();
		return defaults && !defaults->actions.empty();
	}

	int base_order(const scn::world_context& context)
	{
		const auto& defaults = context.input_stack();
		return defaults && defaults->order ? *defaults->order : context.input_layer();
	}

	bool above(const scn::world_context& a, const scn::world_context& b)
	{
		if (base_order(a) != base_order(b)) return base_order(a) > base_order(b);
		return a.desc_index() < b.desc_index();
	}

	// INPUT_STACK.md 6: what the actions do to the worlds is said once per set, so a level that loses keys is not silent.
	void warn_about_declarations(const std::vector<entry>& entries, std::string_view set, const entry* reader)
	{
		const entry* lost = nullptr;
		for (const auto& e : entries) {
			if (e.context->kind() == scn::context_kind::preview) continue;
			const bool lists = lists_update_actions(*e.context);
			if (declares_actions(*e.context) && !lists && warn_once(std::string{ set } + "#declared:" + std::string{ e.context->get_name() })) {
				egLOG_WARN("input", "world '{}' of '{}' declares actions in its input_stack but does not list {}: its layer takes their keys and nobody reads them",
					e.context->get_name(), set, k_update_actions);
			}
			if (reader && &e != reader && above(*reader->context, *e.context)) lost = &e;
			if (!reader && lists && !declares_actions(*e.context) && warn_once(std::string{ set } + "#default-lost:" + std::string{ e.context->get_name() })) {
				egLOG_WARN("input", "world '{}' of '{}' lists {} but gets no actions: another world declares its own, which replaces the default",
					e.context->get_name(), set, k_update_actions);
			}
		}
		if (lost && warn_once(std::string{ set } + "#below")) {
			egLOG_WARN("input", "set '{}': world '{}' reads every action by default and takes their keys; the worlds below it, '{}' among them, no longer get those raw keys "
				"(declare the actions in a world's input_stack to say who reads which)", set, reader->context->get_name(), lost->context->get_name());
		}
	}

	// A world reads the actions its layers declare (INPUT_STACK.md 3.2). While no world of the set lists any of its own the
	// default serves (M6): the first world in the level file that runs inp::update_actions declares all of them. It keeps the
	// map while it is not bound, so it can let its held actions go and a Step without the binding stays frozen. A set
	// whose other such worlds run the system too, and a set that gets the input while none of its worlds runs it, are
	// warned about once.
	const entry* actions_reader(const std::vector<entry>& entries, std::string_view set, bool have_actions)
	{
		const entry* first = nullptr;
		const entry* fed = nullptr;
		std::size_t readers = 0;
		std::size_t declarers = 0;
		for (const auto& e : entries) {
			// A prefab preview shows content, it plays no game.
			if (e.context->kind() == scn::context_kind::preview) continue;
			if (e.routed.focused && !fed) fed = &e;
			if (!lists_update_actions(*e.context)) continue;
			if (declares_actions(*e.context)) {
				++declarers;
				continue;
			}
			++readers;
			if (!first || e.context->desc_index() < first->context->desc_index()) first = &e;
		}
		// The default is for levels that declare nothing; once a world declares its actions it replaces the default.
		if (declarers) first = nullptr;
		if (!have_actions) return first;
		warn_about_declarations(entries, set, first);
		if (!first && !declarers && fed && warn_once(std::string{ set } + "#system")) {
			egLOG_WARN("input", "world '{}' of '{}' gets the input, but no world of the set lists {}: game actions do nothing there",
				fed->context->get_name(), set, k_update_actions);
		}
		if (readers > 1 && warn_once(std::string{ set } + "#readers")) {
			egLOG_WARN("input", "set '{}': {} worlds list {} and declare no actions; only '{}', the first in the level, gets them "
				"(declare actions in a world's input_stack to give them to another)", set, readers, k_update_actions, first->context->get_name());
		}
		return first;
	}

	bool reads_actions(const entry& e, const entry* reader)
	{
		return &e == reader || (declares_actions(*e.context) && lists_update_actions(*e.context));
	}

	using queued_injections = std::vector<inp::input_router::queued_injection>;

	// What was injected into the world, with the names turned into ids of the map it reads (the map warns once about a name
	// it does not have). A world that reads no actions gets nothing.
	inp::action_injection injection_for(inp::input_router& router, const entry& e, const std::shared_ptr<const inp::action_map>& map, std::string_view set,
		const queued_injections& queued)
	{
		inp::action_injection injection;
		for (const auto& item : queued) {
			if (item.world != e.context->get_name()) continue;
			if (!map) {
				if (router.warn_once(set, "#inject:" + item.world)) {
					egLOG_WARN("input", "inject '{}': world '{}' of '{}' reads no actions, dropped", item.action, item.world, set);
				}
				continue;
			}
			const inp::action_id id = map->id(item.action);
			if (id != inp::invalid_action) injection.edges.push_back({ id, item.edge });
		}
		return injection;
	}

	// A world that does not run inp::update_actions this frame (frozen in a Step) gets its edges later: they wait in the queue.
	void keep_for_later(inp::input_router& router, const entry& e, std::string_view set, const queued_injections& queued)
	{
		for (const auto& item : queued) {
			if (item.world == e.context->get_name()) router.inject({ std::string{ set }, item.world }, item.action, item.edge);
		}
	}

	void warn_about_lost_injections(inp::input_router& router, const std::vector<entry>& entries, std::string_view set, const queued_injections& queued)
	{
		for (const auto& item : queued) {
			const bool there = std::ranges::any_of(entries, [&item](const entry& e) { return e.context->get_name() == item.world; });
			if (!there && router.warn_once(set, "#inject-gone:" + item.world)) {
				egLOG_WARN("input", "inject '{}': no world '{}' in '{}', dropped", item.action, item.world, set);
			}
		}
	}

	// Mouse and keyboard parts of the full state are given out separately: a context may take the
	// keyboard of a focused text field while the mouse belongs to someone else. The cursor position goes to everyone.
	void write(const entry& e, inp::world_input* in, const inp::input_bound& bound, std::shared_ptr<const inp::action_map> actions, inp::action_injection injection)
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
		ctx.insert_or_assign(inp::action_input{ in ? std::move(in->actions) : std::vector<inp::action_reach>{} });
		ctx.insert_or_assign(std::move(injection));
	}

	// The commands of the systems (worlds in the order of their desc_index, each in call order), then those queued from outside.
	void apply_stack_commands(inp::input_router& router, const std::vector<entry>& entries, std::string_view set, inp::input_stack& stack)
	{
		std::vector<const entry*> by_file;
		by_file.reserve(entries.size());
		std::vector<inp::world_ref> worlds;
		worlds.reserve(entries.size());
		for (const auto& e : entries) {
			by_file.push_back(&e);
			worlds.push_back({ std::string{ e.context->get_name() }, e.context->desc_index() });
		}
		std::ranges::stable_sort(by_file, [](const entry* a, const entry* b) { return a->context->desc_index() < b->context->desc_index(); });

		std::vector<inp::stack_command> commands;
		for (const entry* e : by_file) {
			auto* control = e->context->state().ctx().find<inp::input_stack_control>();
			if (!control || control->empty()) continue;
			for (inp::stack_command& command : control->take()) {
				if (command.world.empty() && command.kind != inp::stack_command_kind::set_world_active) command.world = e->context->get_name();
				commands.push_back(std::move(command));
			}
		}
		for (inp::stack_command& command : router.take_commands(set)) {
			commands.push_back(std::move(command));
		}
		inp::apply_commands(stack, commands, worlds);
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

		const std::string_view set_name = set.name();
		// Actions exist in Play and Step (INPUT_STACK.md 3.4): an Edit set keeps every raw key.
		static const std::shared_ptr<const inp::action_map> no_actions;
		const std::shared_ptr<const inp::action_map>& live_actions = set.mode() == ecs::run_mode::edit ? no_actions : actions;
		const entry* reader = actions_reader(entries, set_name, live_actions != nullptr);

		inp::input_stack& stack = router.stack(set.name());
		// A Pause keeps the commands waiting: the layers stay as they are until it goes (INPUT_STACK.md 3.4).
		if (set.mode() != ecs::run_mode::pause) {
			apply_stack_commands(router, entries, set_name, stack);
		}
		std::vector<std::string> present;
		std::vector<std::string> fed;
		for (const auto& e : entries) {
			const std::string name{ e.context->get_name() };
			stack.put(name, e.context->desc_index(), base_layer(e, &e == reader));
			present.push_back(name);
			// Contexts the router handed the full state to; the rest stay neutral as before.
			if (e.routed.focused) fed.push_back(name);
		}
		stack.retain_worlds(present);

		inp::input_router::mouse_hold hold = router.hold(set_name);
		if (!fed.empty() && button_down && !hold.active) {
			// The press (or the first sight of an already pressed button) decides who owns the drag.
			const inp::stack_entry* taker = inp::mouse_taker_entry(stack, fed, cursor);
			hold = { true, taker != nullptr, taker ? target_of(set_name, taker->world) : inp::input_target{}, taker ? taker->layer.name : std::string{} };
			router.set_hold(set_name, hold);
		}

		// An owner that is gone (destroyed, or no longer fed by the target) leaves the held
		// button with nobody until it is released: it must not fall through to the others.
		const inp::drag_hold drag{ hold.active, hold.captured, hold.owner.set == set_name ? hold.owner.context : std::string{}, hold.layer };
		inp::stack_resolution resolved = inp::resolve(stack, fed, cursor, engaged, drag, live_actions.get());
		// The memory of fronts of every layer follows the physical state; the actions learn what is armed (INPUT_STACK.md 5).
		inp::arm_actions(stack, resolved, raw, live_actions.get());

		// A Pause does not run inp::update_actions: the edges wait for it, like the commands of the stack.
		const queued_injections injected = set.mode() == ecs::run_mode::pause ? queued_injections{} : router.take_injected(set_name);
		warn_about_lost_injections(router, entries, set_name, injected);
		const bool stepping = set.mode() == ecs::run_mode::step;
		for (const auto& e : entries) {
			const inp::input_bound bound{ e.routed.focused, stepping && !e.routed.focused };
			const auto& read = reads_actions(e, reader) ? live_actions : no_actions;
			inp::action_injection injection;
			if (bound.frozen) {
				keep_for_later(router, e, set_name, injected);
			} else {
				injection = injection_for(router, e, read, set_name, injected);
			}
			write(e, resolved.find(e.context->get_name()), bound, read, std::move(injection));
		}
	}
}

void gs::route_input(inp::input_router& router, scn::context_manager& contexts)
{
	route_input(router, contexts, nullptr);
}

void gs::route_input(inp::input_router& router, scn::context_manager& contexts, const std::shared_ptr<const inp::action_map>& actions)
{
	std::vector<std::string> sets;
	contexts.for_each_set([&router, &actions, &sets](scn::context_set& set) {
		sets.emplace_back(set.name());
		route_set(router, set, actions);
	});
	for (const auto& dropped : router.drop_commands_except(sets)) {
		if (router.warn_once(dropped.set, "#command-set")) {
			egLOG_WARN("input", "input stack commands for set '{}', which is not there: {} command(s) dropped", dropped.set, dropped.edges);
		}
	}
	for (const auto& dropped : router.drop_injected_except(sets)) {
		if (router.warn_once(dropped.set, "#inject-set")) {
			egLOG_WARN("input", "inject into set '{}', which is not there: {} edge(s) dropped", dropped.set, dropped.edges);
		}
	}
	// Cleared after the pass: on the frame the last button goes up the owner still sees it.
	if (!any_mouse_button(router.raw_state())) {
		router.clear_holds();
	}
}
