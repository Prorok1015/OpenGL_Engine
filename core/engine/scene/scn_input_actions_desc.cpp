#include <snk/scene/scn_input_actions_desc.h>
#include <snk/input/inp_input_stack.h>
#include <snk/common/logger/engine_log.h>
#include <optional>

namespace
{
	std::string string_field(const json::object& object, std::string_view key)
	{
		const auto* value = object.if_contains(key);
		return value && value->is_string() ? std::string{ value->as_string() } : std::string{};
	}

	// A field present but not a string is kept as a value no name matches, so build_action_map reports it.
	std::string checked_field(const json::object& object, std::string_view key)
	{
		const auto* value = object.if_contains(key);
		if (!value) return {};
		return value->is_string() ? std::string{ value->as_string() } : std::string{ "<not a string>" };
	}
}

json::value desc::json_codec<scn::input_action_list>::to_json(const scn::input_action_list& actions)
{
	json::array out;
	out.reserve(actions.entries.size());
	for (const auto& action : actions.entries) {
		json::array bindings;
		for (const auto& binding : action.bindings) {
			json::object b;
			if (!binding.key.empty()) b["key"] = binding.key;
			if (!binding.mouse.empty()) b["mouse"] = binding.mouse;
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
		return true;
	}
	std::vector<scn::input_action_entry> parsed;
	parsed.reserve(in.as_array().size());
	for (const auto& item : in.as_array()) {
		scn::input_action_entry action;
		if (const auto* object = item.if_object()) {
			action.name = string_field(*object, "name");
			if (const auto* bindings = object->if_contains("bindings"); bindings && bindings->is_array()) {
				for (const auto& b : bindings->as_array()) {
					scn::input_binding_entry binding;
					if (const auto* bo = b.if_object()) {
						binding.key = checked_field(*bo, "key");
						binding.mouse = checked_field(*bo, "mouse");
					}
					action.bindings.push_back(std::move(binding));
				}
			}
		}
		parsed.push_back(std::move(action));
	}
	actions.entries = std::move(parsed);
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
			if (b.key.empty() == b.mouse.empty()) {
				egLOG_WARN("input", "{}: /actions/{}/bindings/{}: a binding names exactly one of \"key\" and \"mouse\", skipped", file, i, j);
				continue;
			}
			if (!b.key.empty()) {
				if (const auto key = inp::key_from_name(b.key)) {
					bindings.emplace_back(*key);
				} else {
					egLOG_WARN("input", "{}: /actions/{}/bindings/{}/key: unknown key '{}', skipped", file, i, j, b.key);
				}
			} else if (const auto button = inp::mouse_button_from_name(b.mouse)) {
				bindings.emplace_back(*button);
			} else {
				egLOG_WARN("input", "{}: /actions/{}/bindings/{}/mouse: unknown mouse button '{}', skipped", file, i, j, b.mouse);
			}
		}
		if (!map.add(entry.name, std::move(bindings))) {
			egLOG_WARN("input", "{}: /actions/{}/name: action '{}' is already declared, skipped", file, i, entry.name);
		}
	}
	return map;
}
