#include <snk/input/inp_input_stack.h>
#include <algorithm>
#include <iterator>

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
		std::erase_if(out, [&fed](const inp::stack_entry* entry) { return !entry->layer.active || !is_fed(fed, entry->world); });
		return out;
	}

	bool takes_click(const inp::stack_layer_desc& layer, glm::vec2 cursor)
	{
		return layer.block_below || region_holds(layer, cursor);
	}
}

void inp::input_stack::put(std::string_view world, std::uint32_t desc_index, stack_layer_desc layer)
{
	for (stack_entry& entry : m_entries) {
		if (entry.world == world && entry.layer.name == layer.name) {
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
}

void inp::input_stack::remove_world(std::string_view world)
{
	std::erase_if(m_entries, [world](const stack_entry& entry) { return entry.world == world; });
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

std::optional<std::string> inp::mouse_taker(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor)
{
	for (const stack_entry* entry : live_layers(stack, fed)) {
		if (takes_click(entry->layer, cursor)) return entry->world;
	}
	return std::nullopt;
}

inp::stack_resolution inp::resolve(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor, bool engaged, const drag_hold& hold)
{
	stack_resolution out;
	for (const std::string& world : fed) out.worlds[world];
	const std::vector<const stack_entry*> layers = live_layers(stack, fed);

	for (std::size_t k = 0; k < key_count; ++k) {
		const auto key = static_cast<KEYBOARD_BUTTONS>(k);
		for (const stack_entry* entry : layers) {
			world_input& world = out.worlds[entry->world];
			world.keys[k] = true;
			if (takes_key(entry->layer, key)) {
				if (entry->layer.whole_keyboard || entry->layer.text_keys) world.keyboard_focus = true;
				break;
			}
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

	// Buttons and wheel: the holder of a drag only; nobody when the holder of the drag is gone; a drag that started outside
	// the layers never reaches the layer it crosses; else the layer that takes the click (and the ones above it when it
	// blocks), or everybody when no layer takes it.
	if (holder_fed) {
		give(&world_input::buttons, hold.world);
	} else if (!orphaned && drag_from_outside) {
		for (const std::string& name : live_worlds) {
			if (!hit || name != hit->world) give(&world_input::buttons, name);
		}
	} else if (!orphaned && hit) {
		// A region's click is its world's alone, as it was before layers; a modal's is also seen by the layers above it.
		give(&world_input::buttons, hit->world);
		if (hit->layer.block_below) {
			for (std::size_t i = 0; i < hit_at; ++i) give(&world_input::buttons, layers[i]->world);
		}
	} else if (!orphaned) {
		for (const std::string& name : live_worlds) give(&world_input::buttons, name);
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

	for (auto& [name, world] : out.worlds) {
		world.mouse_focus = world.buttons || (world.motion && !engaged);
	}
	return out;
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
