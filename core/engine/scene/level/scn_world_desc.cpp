#include <snk/scene/level/scn_world_desc.h>
#include <snk/common/logger/engine_log.h>
#include <string_view>

namespace
{
	void read_names(const json::object& object, std::string_view key, std::vector<std::string>& out)
	{
		const json::value* value = object.if_contains(key);
		if (!value) return;
		if (!value->is_array()) {
			egLOG_WARN("desc", "world input_stack: '{}' must be an array of strings, ignored", key);
			return;
		}
		for (const json::value& item : value->as_array()) {
			if (item.is_string()) out.emplace_back(item.as_string());
		}
	}

	bool read_flag(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		if (!value) return false;
		if (!value->is_bool()) {
			egLOG_WARN("desc", "world input_stack: '{}' must be true or false, ignored", key);
			return false;
		}
		return value->as_bool();
	}

	scn::input_stack_desc read_input_stack(const json::object& object)
	{
		scn::input_stack_desc out;
		for (const auto& member : object) {
			const std::string_view key = member.key();
			if (key != "order" && key != "active" && key != "keys" && key != "text_keys" && key != "block_below" && key != "actions" && key != "listen") {
				egLOG_WARN("desc", "world input_stack: unknown key '{}' ignored", key);
			}
		}
		if (const json::value* order = object.if_contains("order")) {
			if (order->is_int64()) {
				out.order = static_cast<int>(order->as_int64());
			} else {
				egLOG_WARN("desc", "world input_stack: 'order' must be an integer, ignored");
			}
		}
		if (const json::value* active = object.if_contains("active")) {
			if (active->is_bool()) {
				out.active = active->as_bool();
			} else {
				egLOG_WARN("desc", "world input_stack: 'active' must be true or false, ignored");
			}
		}
		out.block_below = read_flag(object, "block_below");
		out.text_keys = read_flag(object, "text_keys");
		read_names(object, "keys", out.keys);
		read_names(object, "actions", out.actions);
		read_names(object, "listen", out.listen);
		return out;
	}

	json::array names_json(const std::vector<std::string>& names)
	{
		json::array out;
		for (const std::string& name : names) out.emplace_back(json::string(name));
		return out;
	}
}

void scn::world_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("name")) {
		name = json::value_to<std::string>(data.at("name"));
	}

	if (data.contains("systems")) {
		systems = json::value_to<std::vector<std::string>>(data.at("systems"));
	}

	if (data.contains("input_layer")) {
		input_layer = json::value_to<int>(data.at("input_layer"));
	}

	input_stack.reset();
	if (const json::value* stack = data.if_contains("input_stack")) {
		if (stack->is_object()) {
			input_stack = read_input_stack(stack->as_object());
		} else {
			egLOG_WARN("desc", "world '{}': input_stack must be an object, ignored", name);
		}
	}

	scn::prefab_desc::deserialize(desc_system, data);
}

void scn::world_desc::serialize(json::object& data) const
{
	json::object node;
	scn::prefab_desc::serialize(node);
	// The world's own fields go first, the node tree after them.
	data.clear();
	data["__type"] = "world_desc";
	data["name"] = json::value_from(name);
	data["systems"] = json::value_from(systems);
	// The default is not written, so levels saved before the field existed stay byte-identical.
	if (input_layer != 0) {
		data["input_layer"] = input_layer;
	}
	if (input_stack) {
		json::object stack;
		if (input_stack->order) stack["order"] = *input_stack->order;
		if (!input_stack->active) stack["active"] = false;
		if (input_stack->block_below) stack["block_below"] = true;
		if (input_stack->text_keys) stack["text_keys"] = true;
		if (!input_stack->keys.empty()) stack["keys"] = names_json(input_stack->keys);
		if (!input_stack->actions.empty()) stack["actions"] = names_json(input_stack->actions);
		if (!input_stack->listen.empty()) stack["listen"] = names_json(input_stack->listen);
		data["input_stack"] = std::move(stack);
	}
	for (auto& member : node) {
		data[member.key()] = std::move(member.value());
	}
}