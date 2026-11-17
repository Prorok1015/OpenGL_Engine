#include <snk/scene/level/scn_prefab_layer.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace json = boost::json;

namespace scn {

	namespace {

		constexpr std::string_view k_node_keys[] = { "__id", "name", "transform", "components", "overrides", "children" };

		bool is_node_key(std::string_view key)
		{
			return std::ranges::find(k_node_keys, key) != std::end(k_node_keys);
		}

		const json::array* array_of(const json::object& node, std::string_view key)
		{
			const auto* value = node.if_contains(key);
			return value && value->is_array() ? &value->as_array() : nullptr;
		}

		const json::object* object_of(const json::object& node, std::string_view key)
		{
			const auto* value = node.if_contains(key);
			return value && value->is_object() ? &value->as_object() : nullptr;
		}

		const json::value* id_value(const json::value& node)
		{
			return node.is_object() ? node.as_object().if_contains("__id") : nullptr;
		}

		json::value transform_layer(const json::object* inherited, const json::object& target)
		{
			if (!inherited) return target;
			json::object fields;
			for (const std::string_view key : { "position", "rotation", "scale" }) {
				const auto* was = inherited->if_contains(key);
				const auto* now = target.if_contains(key);
				if (now && (!was || *was != *now)) fields[key] = *now;
				else if (!now && was) fields[key] = nullptr;
			}
			return fields;
		}

		bool layer_of_node(const json::object& inherited, const json::object& target, json::object& layer, std::string& why,
			const std::string& where);

		bool layer_children(const json::object& inherited, const json::object& target, json::object& layer, std::string& why,
			const std::string& where)
		{
			const json::array empty;
			const json::array* was = array_of(inherited, "children");
			const json::array* now = array_of(target, "children");
			if (!was) was = &empty;
			if (!now) now = &empty;

			json::array entries;
			for (const auto& base_child : *was) {
				const auto* id = id_value(base_child);
				if (!id) { why = std::format("{}: a node of the base has no '__id'", where); return false; }
				const bool kept = std::ranges::any_of(*now, [&] (const json::value& child) {
					const auto* other = id_value(child);
					return other && *other == *id;
				});
				if (!kept) {
					json::object removal;
					removal["__id"] = *id;
					removal["__remove"] = true;
					entries.push_back(std::move(removal));
				}
			}

			json::array target_order;
			json::array resulting_order;
			for (const auto& base_child : *was) {
				const auto* id = id_value(base_child);
				if (std::ranges::any_of(*now, [&] (const json::value& child) { const auto* o = id_value(child); return o && *o == *id; })) {
					resulting_order.push_back(*id);
				}
			}
			for (const auto& child : *now) {
				const auto* id = id_value(child);
				if (!id || !child.is_object()) { why = std::format("{}: a node has no '__id'", where); return false; }
				target_order.push_back(*id);
				const auto same = std::ranges::find_if(*was, [&] (const json::value& base_child) {
					const auto* other = id_value(base_child);
					return other && *other == *id;
				});
				if (same == was->end()) {
					json::object added = child.as_object();
					added["__new"] = true;
					entries.push_back(std::move(added));
					resulting_order.push_back(*id);
				} else if (*same != child) {
					json::object entry;
					entry["__id"] = *id;
					if (!layer_of_node(same->as_object(), child.as_object(), entry, why, where + "/" + std::string{ id->as_string() })) return false;
					entries.push_back(std::move(entry));
				}
			}
			if (target_order != resulting_order) layer["__order"] = std::move(target_order);
			if (!entries.empty()) layer["children"] = std::move(entries);
			return true;
		}

		bool layer_of_node(const json::object& inherited, const json::object& target, json::object& layer, std::string& why,
			const std::string& where)
		{
			for (const auto& member : target) {
				if (!is_node_key(member.key()) && inherited.if_contains(member.key()) == nullptr) {
					why = std::format("{}: the key '{}' has no place in a layer", where, member.key());
					return false;
				}
			}

			const auto* was_name = inherited.if_contains("name");
			const auto* now_name = target.if_contains("name");
			if (now_name && (!was_name || *was_name != *now_name)) layer["name"] = *now_name;

			const auto* was_transform = object_of(inherited, "transform");
			const auto* now_transform = object_of(target, "transform");
			if (!now_transform && was_transform) layer["transform"] = nullptr;
			else if (now_transform && (!was_transform || *was_transform != *now_transform)) {
				layer["transform"] = transform_layer(was_transform, *now_transform);
			}

			json::object components;
			const json::object none;
			const json::object& was_components = object_of(inherited, "components") ? *object_of(inherited, "components") : none;
			const json::object& now_components = object_of(target, "components") ? *object_of(target, "components") : none;
			for (const auto& member : now_components) {
				const auto* was = was_components.if_contains(member.key());
				if (!was || *was != member.value()) components[member.key()] = member.value();
			}
			for (const auto& member : was_components) {
				if (!now_components.contains(member.key())) components[member.key()] = nullptr;
			}
			if (!components.empty()) layer["components"] = std::move(components);

			const json::array no_records;
			const json::array& was_records = array_of(inherited, "overrides") ? *array_of(inherited, "overrides") : no_records;
			const json::array& now_records = array_of(target, "overrides") ? *array_of(target, "overrides") : no_records;
			if (now_records.size() < was_records.size() || !std::equal(was_records.begin(), was_records.end(), now_records.begin())) {
				why = std::format("{}: the override records of the base cannot be taken out or changed in a layer", where);
				return false;
			}
			if (now_records.size() > was_records.size()) {
				layer["overrides"] = json::array(now_records.begin() + static_cast<std::ptrdiff_t>(was_records.size()), now_records.end());
			}

			return layer_children(inherited, target, layer, why, where);
		}
	}

	std::optional<json::object> make_prefab_layer(const json::object& inherited, const json::object& target, std::string& why)
	{
		const auto* was_id = inherited.if_contains("__id");
		const auto* now_id = target.if_contains("__id");
		if ((was_id == nullptr) != (now_id == nullptr) || (was_id && *was_id != *now_id)) {
			why = "the root of the prefab has another '__id' than its base";
			return std::nullopt;
		}
		json::object layer;
		if (!layer_of_node(inherited, target, layer, why, "")) return std::nullopt;
		layer.erase("__id");
		return layer;
	}

	std::string write_prefab_layer(desc::desc_system& descs, res::resource_system& resources, const prefab_desc& prefab, json::object& body)
	{
		const res::tag& parent_tag = prefab.get_parent();
		if (!parent_tag.is_valid()) return "the prefab has no '__parent'";
		const auto parent = resources.require_sync<desc::desc_base>(parent_tag).get_sync();
		if (!parent) return std::format("the base '{}' did not load", parent_tag.view());

		const auto stamped = [&] (json::object object) {
			descs.stamp_tree(object);
			return object;
		};
		json::object inherited;
		try {
			parent->serialize(inherited);
		} catch (const std::exception& e) {
			return std::format("the base '{}' cannot be written as a file: {}", parent_tag.view(), e.what());
		}
		inherited = stamped(std::move(inherited));
		const json::object target = stamped(body);

		std::string why;
		auto layer = make_prefab_layer(inherited, target, why);
		if (!layer) return why;

		json::object file;
		for (const auto& member : body) {
			if (member.key().starts_with("__")) file.emplace(member.key(), member.value());
		}
		file["__parent"] = json::value_from(parent_tag);
		file["__layer"] = 2;
		for (auto& member : *layer) file.emplace(member.key(), std::move(member.value()));

		try {
			const auto probe = descs.create_instance(prefab.get_type(), prefab.get_tag());
			if (!probe) return "the prefab type is not registered";
			parent->copy_to(*probe);
			probe->set_parent(parent_tag);
			probe->deserialize(descs, file);
			json::object again;
			probe->serialize(again);
			if (stamped(std::move(again)) != target) return "the layer does not read back as the edited prefab";
		} catch (const std::exception& e) {
			return std::format("the layer does not read back: {}", e.what());
		}
		body = std::move(file);
		return {};
	}
}
