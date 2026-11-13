#include <snk/scene/scn_input_actions_desc.h>
#include <snk/input/inp_input_stack.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_json_storage.hpp>
#include <algorithm>
#include <optional>

namespace
{
	std::string string_field(const json::object& object, std::string_view key)
	{
		const auto* value = object.if_contains(key);
		return value && value->is_string() ? std::string{ value->as_string() } : std::string{};
	}

	std::vector<scn::input_action_entry> parse_entries(const json::array& items)
	{
		std::vector<scn::input_action_entry> parsed;
		parsed.reserve(items.size());
		for (const auto& item : items) {
			scn::input_action_entry action;
			if (const auto* object = item.if_object()) {
				action.name = string_field(*object, "name");
				if (const auto* bindings = object->if_contains("bindings"); bindings && bindings->is_array()) {
					for (const auto& b : bindings->as_array()) {
						scn::input_binding_entry binding;
						if (const auto* bo = b.if_object()) {
							binding.key = string_field(*bo, "key");
							binding.mouse = string_field(*bo, "mouse");
							binding.gamepad = string_field(*bo, "gamepad");
							binding.axis = string_field(*bo, "axis");
							binding.key_not_text = bo->contains("key") && !bo->at("key").is_string();
							binding.mouse_not_text = bo->contains("mouse") && !bo->at("mouse").is_string();
							binding.gamepad_not_text = bo->contains("gamepad") && !bo->at("gamepad").is_string();
							binding.axis_not_text = bo->contains("axis") && !bo->at("axis").is_string();
						}
						action.bindings.push_back(std::move(binding));
					}
				}
			}
			parsed.push_back(std::move(action));
		}
		return parsed;
	}
}

json::value desc::json_codec<scn::input_action_list>::to_json(const scn::input_action_list& actions)
{
	if (actions.source.is_array() && parse_entries(actions.source.as_array()) == actions.entries) return actions.source;
	if (!actions.source.is_null() && !actions.source.is_array() && actions.entries.empty()) return actions.source;
	json::array out;
	out.reserve(actions.entries.size());
	for (const auto& action : actions.entries) {
		json::array bindings;
		for (const auto& binding : action.bindings) {
			json::object b;
			if (!binding.key.empty()) b["key"] = binding.key;
			if (!binding.mouse.empty()) b["mouse"] = binding.mouse;
			if (!binding.gamepad.empty()) b["gamepad"] = binding.gamepad;
			if (!binding.axis.empty()) b["axis"] = binding.axis;
			bindings.push_back(std::move(b));
		}
		json::object a;
		a["name"] = action.name;
		a["bindings"] = std::move(bindings);
		out.push_back(std::move(a));
	}
	return out;
}

bool desc::json_codec<scn::input_action_list>::from_json(scn::input_action_list& actions, const json::value& in)
{
	if (!in.is_array()) {
		egLOG_WARN("input", "input_actions_desc: /actions: expected an array, the file has no actions");
		actions.entries.clear();
		actions.source = in;
		ds::keep_in_default_resource(actions.source);
		return true;
	}
	actions.entries = parse_entries(in.as_array());
	actions.source = in;
	ds::keep_in_default_resource(actions.source);
	return true;
}

inp::action_map scn::build_action_map(const input_actions_desc& desc, std::string_view source)
{
	inp::action_map map;
	const std::string_view file = source.empty() ? std::string_view{ "input actions" } : source;
	for (size_t i = 0; i < desc.actions.entries.size(); ++i) {
		const auto& entry = desc.actions.entries[i];
		if (entry.name.empty()) {
			egLOG_WARN("input", "{}: /actions/{}: an action without a name, skipped", file, i);
			continue;
		}
		std::vector<inp::action_binding> bindings;
		for (size_t j = 0; j < entry.bindings.size(); ++j) {
			const auto& b = entry.bindings[j];
			struct field { const char* name; const std::string& value; bool not_text; };
			const field fields[] = {
				{ "key", b.key, b.key_not_text }, { "mouse", b.mouse, b.mouse_not_text },
				{ "gamepad", b.gamepad, b.gamepad_not_text }, { "axis", b.axis, b.axis_not_text } };
			const field* broken = nullptr;
			for (const field& f : fields) {
				if (f.not_text) {
					broken = &f;
					break;
				}
			}
			if (broken) {
				egLOG_WARN("input", "{}: /actions/{}/bindings/{}/{}: not a string, skipped", file, i, j, broken->name);
				continue;
			}
			const auto named = std::ranges::count_if(fields, [](const field& f) { return !f.value.empty(); });
			if (named != 1) {
				egLOG_WARN("input", "{}: /actions/{}/bindings/{}: a binding names exactly one of \"key\", \"mouse\", \"gamepad\" and \"axis\", skipped", file, i, j);
				continue;
			}
			if (!b.key.empty()) {
				if (const auto key = inp::key_from_name(b.key)) {
					bindings.emplace_back(*key);
				} else {
					egLOG_WARN("input", "{}: /actions/{}/bindings/{}/key: unknown key '{}', skipped", file, i, j, b.key);
				}
			} else if (!b.mouse.empty()) {
				if (const auto button = inp::mouse_button_from_name(b.mouse)) {
					bindings.emplace_back(*button);
				} else {
					egLOG_WARN("input", "{}: /actions/{}/bindings/{}/mouse: unknown mouse button '{}', skipped", file, i, j, b.mouse);
				}
			} else if (!b.gamepad.empty()) {
				if (const auto button = inp::gamepad_button_from_name(b.gamepad)) {
					bindings.emplace_back(*button);
				} else {
					egLOG_WARN("input", "{}: /actions/{}/bindings/{}/gamepad: unknown gamepad button '{}', skipped", file, i, j, b.gamepad);
				}
			} else if (const auto axis = inp::gamepad_axis_from_name(b.axis)) {
				bindings.emplace_back(*axis);
			} else {
				egLOG_WARN("input", "{}: /actions/{}/bindings/{}/axis: unknown gamepad axis '{}', skipped", file, i, j, b.axis);
			}
		}
		if (!map.add(entry.name, std::move(bindings))) {
			egLOG_WARN("input", "{}: /actions/{}/name: action '{}' is already declared, skipped", file, i, entry.name);
		}
	}
	return map;
}

namespace
{
	// A number of the object, reported by build_gamepad_config when it is not one.
	bool read_number(const json::object& object, std::string_view key, float& out, bool& present, bool& wrong_type)
	{
		const auto* value = object.if_contains(key);
		if (!value) return false;
		present = true;
		if (!value->is_number()) {
			wrong_type = true;
			return false;
		}
		out = static_cast<float>(std::clamp(value->to_number<double>(), -1e9, 1e9));
		return true;
	}
}

json::value desc::json_codec<scn::input_gamepad_settings>::to_json(const scn::input_gamepad_settings& settings)
{
	scn::input_gamepad_settings reread;
	if (settings.source.is_object()) {
		from_json(reread, settings.source);
		if (reread == settings) return settings.source;
	}
	// Nothing to write and no key in the file: null, which the desc leaves out (no "gamepad": {} added to a file without one).
	if (!settings.has_deadzone && !settings.has_press_threshold) return json::value();
	json::object out;
	if (settings.has_deadzone) out["deadzone"] = settings.config.deadzone;
	if (settings.has_press_threshold) out["press_threshold"] = settings.config.press_threshold;
	return out;
}

bool desc::json_codec<scn::input_gamepad_settings>::from_json(scn::input_gamepad_settings& settings, const json::value& in)
{
	settings = {};
	settings.source = in;
	ds::keep_in_default_resource(settings.source);
	const json::object* object = in.if_object();
	if (!object) {
		settings.not_object = true;
		return true;
	}
	settings.has_deadzone = false;
	settings.has_press_threshold = false;
	read_number(*object, "deadzone", settings.config.deadzone, settings.has_deadzone, settings.deadzone_not_number);
	read_number(*object, "press_threshold", settings.config.press_threshold, settings.has_press_threshold, settings.press_threshold_not_number);
	return true;
}

inp::gamepad_config scn::build_gamepad_config(const input_actions_desc& desc, std::string_view source)
{
	const std::string_view file = source.empty() ? std::string_view{ "input actions" } : source;
	const input_gamepad_settings& settings = desc.gamepad;
	inp::gamepad_config config;
	if (settings.not_object) egLOG_WARN("input", "{}: /gamepad: expected an object, the defaults stay", file);
	const auto take = [&](const char* key, float value, bool present, bool not_number, float& target) {
		if (!present) return;
		if (not_number) {
			egLOG_WARN("input", "{}: /gamepad/{}: not a number, the default stays", file, key);
		} else if (!(value >= 0.f && value <= 1.f)) {
			egLOG_WARN("input", "{}: /gamepad/{}: {} is out of 0..1, the default stays", file, key, value);
		} else {
			target = value;
		}
	};
	take("deadzone", settings.config.deadzone, settings.has_deadzone, settings.deadzone_not_number, config.deadzone);
	take("press_threshold", settings.config.press_threshold, settings.has_press_threshold, settings.press_threshold_not_number, config.press_threshold);
	return config;
}
