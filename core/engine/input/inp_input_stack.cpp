#include <snk/input/inp_input_stack.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <format>
#include <bitset>
#include <iterator>
#include <variant>

namespace
{
	using inp::KEYBOARD_BUTTONS;

	constexpr std::string_view k_key_names[] = {
		"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
		"SPACE", "APOSTROPHE", "COMMA", "MINUS", "PERIOD", "SLASH",
		"K_0", "K_1", "K_2", "K_3", "K_4", "K_5", "K_6", "K_7", "K_8", "K_9",
		"SEMICOLON", "EQUAL",
		"ESCAPE", "ENTER", "TAB", "BACKSPACE", "INSERT", "DELETE",
		"RIGHT", "LEFT", "DOWN", "UP", "PAGE_UP", "PAGE_DOWN", "HOME", "END",
		"CAPS_LOCK", "SCROLL_LOCK", "NUM_LOCK", "PRINT_SCREEN", "PAUSE",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "F25",
		"KP_0", "KP_1", "KP_2", "KP_3", "KP_4", "KP_5", "KP_6", "KP_7", "KP_8", "KP_9", "KP_DECIMAL", "KP_DIVIDE", "KP_MULTIPLY", "KP_SUBTRACT", "KP_ADD", "KP_ENTER", "KP_EQUAL",
		"LEFT_SHIFT", "LEFT_CONTROL", "LEFT_ALT", "LEFT_SUPER", "RIGHT_SHIFT", "RIGHT_CONTROL", "RIGHT_ALT", "RIGHT_SUPER", "MENU",
		"GRAVE_ACCENT", "LEFT_BRACKET", "BACKSLASH", "RIGHT_BRACKET", "WORLD_1", "WORLD_2",
	};
	static_assert(std::size(k_key_names) == inp::key_count, "a key was added to KEYBOARD_BUTTONS: name it here");

	constexpr std::string_view k_mouse_names[] = { "LEFT", "RIGHT", "MIDDLE", "BTN_4", "BTN_5", "BTN_6", "BTN_7", "BTN_8" };
	static_assert(std::size(k_mouse_names) == static_cast<std::size_t>(inp::MOUSE_BUTTONS::LAST), "a button was added to MOUSE_BUTTONS: name it here");

	bool region_holds(const inp::stack_layer_desc& layer, glm::vec2 pos)
	{
		return std::ranges::any_of(layer.regions, [pos](const glm::ivec4& r) {
			return pos.x >= r.x && pos.x <= r.z && pos.y >= r.y && pos.y <= r.w;
		});
	}

	bool takes_key(const inp::stack_layer_desc& layer, KEYBOARD_BUTTONS key)
	{
		return layer.block_below || layer.whole_keyboard || (layer.text_keys && inp::is_text_key(key))
			|| std::ranges::find(layer.keys, key) != layer.keys.end();
	}

	bool is_fed(const std::vector<std::string>& fed, const std::string& world)
	{
		return std::ranges::find(fed, world) != fed.end();
	}

	// The layers that can receive anything, top first.
	std::vector<const inp::stack_entry*> live_layers(const inp::input_stack& stack, const std::vector<std::string>& fed)
	{
		std::vector<const inp::stack_entry*> out = stack.ordered();
		std::erase_if(out, [&fed, &stack](const inp::stack_entry* entry) {
			return !entry->layer.active || !stack.world_active(entry->world) || !is_fed(fed, entry->world);
		});
		return out;
	}

	bool takes_click(const inp::stack_layer_desc& layer, glm::vec2 cursor)
	{
		return layer.block_below || region_holds(layer, cursor);
	}

	// An action a layer names, with the bindings of the map.
	struct declared_action
	{
		inp::action_id id = inp::invalid_action;
		std::bitset<inp::key_count> keys;
		std::bitset<inp::mouse_count> buttons;
		bool mouse = false;
	};

	std::vector<declared_action> declared_by(const inp::stack_layer_desc& layer, const inp::action_map* map)
	{
		std::vector<declared_action> out;
		if (!map) return out;
		const auto declare = [&out](inp::action_id id, const inp::action& action) {
			declared_action declared;
			declared.id = id;
			for (const inp::action_binding& binding : action.bindings) {
				if (const auto key = binding.key()) {
					declared.keys.set(static_cast<std::size_t>(*key));
				} else if (const auto button = binding.mouse_button()) {
					declared.mouse = true;
					if (static_cast<std::size_t>(*button) < inp::mouse_count) declared.buttons.set(static_cast<std::size_t>(*button));
				}
			}
			out.push_back(declared);
		};
		if (layer.all_actions) {
			const auto list = map->actions();
			for (std::size_t id = 0; id < list.size(); ++id) declare(static_cast<inp::action_id>(id), list[id]);
			return out;
		}
		for (const std::string& name : layer.actions) {
			const inp::action_id id = map->id(name);
			if (const inp::action* action = map->find(id)) declare(id, *action);
		}
		return out;
	}
}

void inp::input_stack::put(std::string_view world, std::uint32_t desc_index, stack_layer_desc layer)
{
	for (stack_entry& entry : m_entries) {
		if (entry.world == world && entry.layer.name == layer.name) {
			if (entry.desc_index != desc_index) {
				forget_memory(world, layer.name);
				m_off_worlds.erase(std::string{ world });
			}
			entry.desc_index = desc_index;
			entry.layer = std::move(layer);
			return;
		}
	}
	m_entries.push_back({ std::string{ world }, desc_index, std::move(layer), m_added++ });
}

void inp::input_stack::retain_worlds(const std::vector<std::string>& worlds)
{
	std::erase_if(m_entries, [&worlds](const stack_entry& entry) { return !is_fed(worlds, entry.world); });
	std::erase_if(m_memory, [&worlds](const auto& item) { return !is_fed(worlds, item.first.first); });
	std::erase_if(m_off_worlds, [&worlds](const std::string& world) { return !is_fed(worlds, world); });
}

void inp::input_stack::remove_world(std::string_view world)
{
	std::erase_if(m_entries, [world](const stack_entry& entry) { return entry.world == world; });
	std::erase_if(m_memory, [world](const auto& item) { return item.first.first == world; });
	if (const auto it = m_off_worlds.find(world); it != m_off_worlds.end()) m_off_worlds.erase(it);
}

void inp::input_stack::set_world_active(std::string_view world, bool active)
{
	if (active) {
		if (const auto it = m_off_worlds.find(world); it != m_off_worlds.end()) m_off_worlds.erase(it);
	} else {
		m_off_worlds.emplace(world);
	}
}

bool inp::input_stack::world_active(std::string_view world) const
{
	return !m_off_worlds.contains(world);
}

inp::stack_snapshot inp::input_stack::snapshot() const
{
	stack_snapshot saved;
	for (const stack_entry& entry : m_entries) {
		if (entry.layer.owner != layer_owner::engine) saved.layers.push_back({ entry.world, entry.layer });
	}
	saved.off_worlds.assign(m_off_worlds.begin(), m_off_worlds.end());
	return saved;
}

inp::stack_restore_report inp::input_stack::restore(const stack_snapshot& saved, const std::vector<world_ref>& worlds)
{
	stack_restore_report report;
	for (const world_ref& world : worlds) {
		if (find(world.name, base_layer_name)) continue;
		stack_layer_desc placeholder;
		placeholder.name = base_layer_name;
		placeholder.active = false;
		put(world.name, world.desc_index, std::move(placeholder));
	}
	for (const stack_snapshot::layer& item : saved.layers) {
		const auto world = std::ranges::find(worlds, item.world, &world_ref::name);
		if (world == worlds.end()) {
			++report.skipped;
			continue;
		}
		put(item.world, world->desc_index, item.desc);
		++report.restored;
	}
	for (const std::string& name : saved.off_worlds) {
		if (std::ranges::find(worlds, name, &world_ref::name) != worlds.end()) {
			set_world_active(name, false);
		} else {
			++report.off_worlds_skipped;
		}
	}
	return report;
}

bool inp::input_stack::warn_once(std::string key)
{
	return m_warned.insert(std::move(key)).second;
}

inp::stack_layer_desc* inp::input_stack::find(std::string_view world, std::string_view name)
{
	const auto it = std::ranges::find_if(m_entries, [&](const stack_entry& entry) { return entry.world == world && entry.layer.name == name; });
	return it == m_entries.end() ? nullptr : &it->layer;
}

bool inp::input_stack::remove(std::string_view world, std::string_view name)
{
	const auto it = std::ranges::find_if(m_entries, [&](const stack_entry& entry) { return entry.world == world && entry.layer.name == name; });
	if (it == m_entries.end()) return false;
	m_entries.erase(it);
	forget_memory(world, name);
	return true;
}

void inp::input_stack::remove_owner(layer_owner owner)
{
	for (const stack_entry& entry : m_entries) {
		if (entry.layer.owner == owner) forget_memory(entry.world, entry.layer.name);
	}
	std::erase_if(m_entries, [owner](const stack_entry& entry) { return entry.layer.owner == owner; });
}

inp::layer_memory& inp::input_stack::memory(std::string_view world, std::string_view layer)
{
	return m_memory[{ std::string{ world }, std::string{ layer } }];
}

const inp::layer_memory* inp::input_stack::find_memory(std::string_view world, std::string_view layer) const
{
	const auto it = m_memory.find({ std::string{ world }, std::string{ layer } });
	return it == m_memory.end() ? nullptr : &it->second;
}

void inp::input_stack::forget_memory(std::string_view world, std::string_view layer)
{
	m_memory.erase({ std::string{ world }, std::string{ layer } });
}

const inp::stack_layer_desc* inp::input_stack::find(std::string_view world, std::string_view name) const
{
	const auto it = std::ranges::find_if(m_entries, [&](const stack_entry& entry) { return entry.world == world && entry.layer.name == name; });
	return it == m_entries.end() ? nullptr : &it->layer;
}

std::vector<const inp::stack_entry*> inp::input_stack::ordered() const
{
	std::vector<const stack_entry*> out;
	out.reserve(m_entries.size());
	for (const stack_entry& entry : m_entries) out.push_back(&entry);
	std::ranges::sort(out, [](const stack_entry* a, const stack_entry* b) {
		if (a->layer.order != b->layer.order) return a->layer.order > b->layer.order;
		if (a->desc_index != b->desc_index) return a->desc_index < b->desc_index;
		return a->added > b->added;
	});
	return out;
}

const inp::stack_entry* inp::mouse_taker_entry(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor)
{
	for (const stack_entry* entry : live_layers(stack, fed)) {
		if (takes_click(entry->layer, cursor)) return entry;
	}
	return nullptr;
}

std::optional<std::string> inp::mouse_taker(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor)
{
	if (const stack_entry* entry = mouse_taker_entry(stack, fed, cursor)) return entry->world;
	return std::nullopt;
}

inp::stack_resolution inp::resolve(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor, bool engaged, const drag_hold& hold,
	const action_map* actions)
{
	stack_resolution out;
	for (const std::string& world : fed) out.worlds[world];
	const std::vector<const stack_entry*> layers = live_layers(stack, fed);

	std::vector<std::vector<declared_action>> declared;
	declared.reserve(layers.size());
	for (const stack_entry* entry : layers) declared.push_back(declared_by(entry->layer, actions));
	if (actions) {
		for (auto& [name, world] : out.worlds) world.actions.assign(actions->size(), action_reach{});
	}
	out.layers.resize(layers.size());
	for (std::size_t i = 0; i < layers.size(); ++i) {
		out.layers[i].world = layers[i]->world;
		out.layers[i].layer = layers[i]->layer.name;
	}

	for (std::size_t k = 0; k < key_count; ++k) {
		const auto key = static_cast<KEYBOARD_BUTTONS>(k);
		for (std::size_t i = 0; i < layers.size(); ++i) {
			const stack_entry* entry = layers[i];
			world_input& world = out.worlds[entry->world];
			world.keys[k] = true;
			out.layers[i].keys.set(k);
			bool takes_action = false;
			for (const declared_action& action : declared[i]) {
				if (!action.keys.test(k)) continue;
				world.actions[action.id].keys.set(k);
				takes_action = true;
			}
			const bool takes_own = takes_key(entry->layer, key);
			if (takes_own && (entry->layer.whole_keyboard || entry->layer.text_keys)) world.keyboard_focus = true;
			if (takes_own || takes_action) break;
		}
	}

	std::vector<std::string> live_worlds;
	for (const stack_entry* entry : layers) {
		if (std::ranges::find(live_worlds, entry->world) == live_worlds.end()) live_worlds.push_back(entry->world);
	}
	const bool holder_fed = hold.active && hold.captured && is_fed(live_worlds, hold.world);
	const bool orphaned = hold.active && hold.captured && !holder_fed;
	const bool drag_from_outside = hold.active && !hold.captured;

	const stack_entry* hit = nullptr;
	std::size_t hit_at = 0;
	for (std::size_t i = 0; i < layers.size(); ++i) {
		if (takes_click(layers[i]->layer, cursor)) {
			hit = layers[i];
			hit_at = i;
			break;
		}
	}

	const auto give = [&out](bool world_input::*part, const std::string& name) { out.worlds[name].*part = true; };
	// The layers the click or the drag reaches, for the mouse bindings of actions: a modal layer of a world hides the
	// mouse actions of the layers of that world below it.
	std::vector<bool> layer_buttons(layers.size(), false);

	// Buttons and wheel: the holder of a drag only; nobody when the holder of the drag is gone; a drag that started outside
	// the layers never reaches the layer it crosses; else the layer that takes the click (and the ones above it when it
	// blocks), or everybody when no layer takes it.
	if (holder_fed) {
		give(&world_input::buttons, hold.world);
		std::size_t holder_at = layers.size();
		for (std::size_t i = 0; i < layers.size() && !hold.layer.empty(); ++i) {
			if (layers[i]->world == hold.world && layers[i]->layer.name == hold.layer) {
				holder_at = i;
				break;
			}
		}
		for (std::size_t i = 0; i < layers.size(); ++i) {
			if (layers[i]->world != hold.world) continue;
			if (hold.layer.empty() || (holder_at < layers.size() && i <= holder_at)) layer_buttons[i] = true;
		}
	} else if (!orphaned && drag_from_outside) {
		for (const std::string& name : live_worlds) {
			if (!hit || name != hit->world) give(&world_input::buttons, name);
		}
		for (std::size_t i = 0; i < layers.size(); ++i) {
			if (!hit || layers[i]->world != hit->world) layer_buttons[i] = true;
		}
	} else if (!orphaned && hit) {
		// A region's click is its world's alone, as it was before layers; a modal's is also seen by the layers above it.
		give(&world_input::buttons, hit->world);
		layer_buttons[hit_at] = true;
		if (hit->layer.block_below) {
			for (std::size_t i = 0; i < hit_at; ++i) give(&world_input::buttons, layers[i]->world);
		}
		for (std::size_t i = 0; i < hit_at; ++i) {
			if (layers[i]->world == hit->world || hit->layer.block_below) layer_buttons[i] = true;
		}
	} else if (!orphaned) {
		for (const std::string& name : live_worlds) give(&world_input::buttons, name);
		layer_buttons.assign(layers.size(), true);
	}

	// Motion is taken by a drag a layer holds or by a layer that blocks, never by a region: a crosshair region must not
	// stop the camera.
	if (holder_fed) {
		give(&world_input::motion, hold.world);
	} else if (!orphaned && drag_from_outside) {
		for (const std::string& name : live_worlds) {
			if (!hit || name != hit->world) give(&world_input::motion, name);
		}
	} else if (!orphaned) {
		for (const stack_entry* entry : layers) {
			give(&world_input::motion, entry->world);
			if (entry->layer.block_below) break;
		}
	}

	for (std::size_t i = 0; i < layers.size(); ++i) {
		out.layers[i].buttons = layer_buttons[i];
		if (!layer_buttons[i]) continue;
		world_input& world = out.worlds[layers[i]->world];
		for (const declared_action& action : declared[i]) {
			if (action.mouse) world.actions[action.id].buttons = true;
		}
	}

	for (auto& [name, world] : out.worlds) {
		world.mouse_focus = world.buttons || (world.motion && !engaged);
	}
	return out;
}

void inp::arm_actions(input_stack& stack, stack_resolution& resolved, const input_state& physical, const action_map* actions)
{
	std::bitset<key_count> held;
	for (std::size_t k = 0; k < key_count && k < physical.keyboard.size(); ++k) held[k] = physical.keyboard[k];
	std::bitset<mouse_count> held_buttons;
	for (std::size_t b = 0; b < mouse_count && b < physical.mouse.size(); ++b) held_buttons[b] = physical.mouse[b];

	for (const stack_entry* entry : stack.ordered()) {
		layer_memory& memory = stack.memory(entry->world, entry->layer.name);
		if (!memory.initialized) {
			memory.initialized = true;
			memory.armed_keys = ~held;
			memory.armed_buttons = ~held_buttons;
		}

		std::bitset<key_count> reached_keys;
		bool reached_buttons = false;
		const auto reach = std::ranges::find_if(resolved.layers, [entry](const layer_reach& item) {
			return item.world == entry->world && item.layer == entry->layer.name;
		});
		if (reach != resolved.layers.end()) {
			reached_keys = reach->keys;
			reached_buttons = reach->buttons;
			if (world_input* world = resolved.find(entry->world)) {
				for (const declared_action& action : declared_by(entry->layer, actions)) {
					if (action.id >= world->actions.size()) continue;
					action_reach& armed = world->actions[action.id];
					armed.armed_keys |= action.keys & reached_keys & memory.armed_keys;
					if (reached_buttons) armed.armed_buttons |= action.buttons & memory.armed_buttons;
				}
			}
		}

		const std::bitset<mouse_count> reached_mask = reached_buttons ? ~std::bitset<mouse_count>() : std::bitset<mouse_count>();
		memory.armed_keys = ~held | (held & reached_keys & memory.armed_keys);
		memory.armed_buttons = ~held_buttons | (held_buttons & reached_mask & memory.armed_buttons);
	}
}

bool inp::is_text_key(KEYBOARD_BUTTONS key)
{
	const auto in = [key](KEYBOARD_BUTTONS first, KEYBOARD_BUTTONS last) { return key >= first && key <= last; };
	return in(KEYBOARD_BUTTONS::A, KEYBOARD_BUTTONS::EQUAL)
		|| key == KEYBOARD_BUTTONS::BACKSPACE || key == KEYBOARD_BUTTONS::DELETE
		|| in(KEYBOARD_BUTTONS::RIGHT, KEYBOARD_BUTTONS::UP)
		|| key == KEYBOARD_BUTTONS::HOME || key == KEYBOARD_BUTTONS::END
		|| in(KEYBOARD_BUTTONS::KP_0, KEYBOARD_BUTTONS::KP_ADD) || key == KEYBOARD_BUTTONS::KP_EQUAL
		|| in(KEYBOARD_BUTTONS::GRAVE_ACCENT, KEYBOARD_BUTTONS::RIGHT_BRACKET);
}

std::optional<inp::KEYBOARD_BUTTONS> inp::key_from_name(std::string_view name)
{
	const auto it = std::ranges::find(k_key_names, name);
	if (it == std::end(k_key_names)) return std::nullopt;
	return static_cast<KEYBOARD_BUTTONS>(std::distance(std::begin(k_key_names), it));
}

std::string_view inp::key_code_name(KEYBOARD_BUTTONS key)
{
	const auto index = static_cast<std::size_t>(key);
	return index < std::size(k_key_names) ? k_key_names[index] : std::string_view{};
}

std::optional<inp::MOUSE_BUTTONS> inp::mouse_button_from_name(std::string_view name)
{
	const auto it = std::ranges::find(k_mouse_names, name);
	if (it == std::end(k_mouse_names)) return std::nullopt;
	return static_cast<MOUSE_BUTTONS>(std::distance(std::begin(k_mouse_names), it));
}

std::string_view inp::mouse_button_name(MOUSE_BUTTONS button)
{
	const auto index = static_cast<std::size_t>(button);
	return index < std::size(k_mouse_names) ? k_mouse_names[index] : std::string_view{};
}

void inp::input_stack_control::push(stack_layer_desc layer)
{
	m_commands.push_back({ .kind = stack_command_kind::push, .layer = std::move(layer) });
}

void inp::input_stack_control::replace(stack_layer_desc layer)
{
	m_commands.push_back({ .kind = stack_command_kind::replace, .layer = std::move(layer) });
}

void inp::input_stack_control::remove(std::string name)
{
	m_commands.push_back({ .kind = stack_command_kind::remove, .name = std::move(name) });
}

void inp::input_stack_control::set_active(std::string name, bool active)
{
	m_commands.push_back({ .kind = stack_command_kind::set_active, .name = std::move(name), .active = active });
}

void inp::input_stack_control::set_order(std::string name, int order)
{
	m_commands.push_back({ .kind = stack_command_kind::set_order, .name = std::move(name), .order = order });
}

void inp::input_stack_control::set_world_active(std::string world, bool active)
{
	m_commands.push_back({ .kind = stack_command_kind::set_world_active, .world = std::move(world), .active = active });
}

std::vector<inp::stack_command> inp::input_stack_control::take()
{
	return std::exchange(m_commands, {});
}

namespace
{
	constexpr std::string_view k_command_names[] = { "push", "replace", "remove", "set_active", "set_order", "set_world_active" };

	void refuse(inp::input_stack& stack, const inp::stack_command& command, std::string_view why)
	{
		const std::string& layer = command.kind == inp::stack_command_kind::push || command.kind == inp::stack_command_kind::replace
			? command.layer.name : command.name;
		const std::string_view kind = k_command_names[static_cast<std::size_t>(command.kind)];
		if (!stack.warn_once(std::format("{}|{}|{}|{}", kind, command.world, layer, why))) return;
		egLOG_WARN("input", "input stack: {} '{}' in world '{}' refused: {}", kind, layer, command.world, why);
	}
}

std::size_t inp::apply_commands(input_stack& stack, const std::vector<stack_command>& commands, const std::vector<world_ref>& worlds)
{
	std::size_t applied = 0;
	for (const stack_command& command : commands) {
		const auto world = std::ranges::find(worlds, command.world, &world_ref::name);
		if (world == worlds.end()) {
			refuse(stack, command, "no such world in the set");
			continue;
		}
		if (command.kind == stack_command_kind::set_world_active) {
			stack.set_world_active(command.world, command.active);
			++applied;
			continue;
		}

		const bool puts = command.kind == stack_command_kind::push || command.kind == stack_command_kind::replace;
		const std::string& name = puts ? command.layer.name : command.name;
		if (name.empty()) {
			refuse(stack, command, "the layer has no name");
			continue;
		}
		stack_layer_desc* existing = stack.find(command.world, name);
		if (existing && existing->owner != command.source) {
			refuse(stack, command, "the layer belongs to another owner");
			continue;
		}
		if (!existing && command.kind != stack_command_kind::push) {
			refuse(stack, command, "no such layer");
			continue;
		}

		switch (command.kind) {
		case stack_command_kind::push:
		case stack_command_kind::replace: {
			stack_layer_desc layer = command.layer;
			layer.owner = command.source;
			stack.put(command.world, world->desc_index, std::move(layer));
			break;
		}
		case stack_command_kind::remove:
			stack.remove(command.world, name);
			break;
		case stack_command_kind::set_active:
			existing->active = command.active;
			break;
		case stack_command_kind::set_order:
			existing->order = command.order;
			break;
		case stack_command_kind::set_world_active:
			break;
		}
		++applied;
	}
	return applied;
}
