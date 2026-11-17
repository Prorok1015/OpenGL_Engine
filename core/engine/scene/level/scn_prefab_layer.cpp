#include <snk/scene/level/scn_prefab_layer.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_override_record.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <format>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace json = boost::json;

namespace scn {

	namespace {

		constexpr std::string_view k_node_keys[] = { "__id", "name", "transform", "components", "overrides", "children" };

		bool is_node_key(std::string_view key)
		{
			return std::ranges::find(k_node_keys, key) != std::end(k_node_keys);
		}

		// The records of every node as the prefab holds them, before the canonical form serialize writes: a prefix test on the
		// canonical lists would refuse an edit that only made canonicalize reorder or drop a record.
		using records_by_node = std::unordered_map<uint64_t, json::array>;

		void collect_records(const prefab_desc::prefab_node& node, records_by_node& out)
		{
			json::array records;
			for (const auto& record : node.overrides) records.push_back(override_record_to_json(record));
			out[node.node_id] = std::move(records);
			for (const auto& child : node.children) collect_records(child, out);
		}

		uint64_t node_id_of(const json::object& node)
		{
			const auto* id = node.if_contains("__id");
			return id && id->is_string() ? desc::parse_node_id(id->as_string()).value_or(0) : 0;
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

		struct records_pair
		{
			const records_by_node& was;
			const records_by_node& now;
		};

		bool layer_of_node(const json::object& inherited, const json::object& target, json::object& layer, std::string& why,
			const std::string& where, const records_pair& records);

		bool layer_children(const json::object& inherited, const json::object& target, json::object& layer, std::string& why,
			const std::string& where, const records_pair& records)
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
					if (!layer_of_node(same->as_object(), child.as_object(), entry, why, where + "/" + std::string{ id->as_string() }, records)) return false;
					entries.push_back(std::move(entry));
				}
			}
			if (target_order != resulting_order) layer["__order"] = std::move(target_order);
			if (!entries.empty()) layer["children"] = std::move(entries);
			return true;
		}

		bool layer_of_node(const json::object& inherited, const json::object& target, json::object& layer, std::string& why,
			const std::string& where, const records_pair& records)
		{
			for (const auto& member : target) {
				if (!is_node_key(member.key()) && inherited.if_contains(member.key()) == nullptr) {
					why = std::format("{}: the key '{}' has no place in a layer", where, std::string_view{ member.key() });
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
			const auto was_found = records.was.find(node_id_of(inherited));
			const auto now_found = records.now.find(node_id_of(target));
			const json::array& was_records = was_found != records.was.end() ? was_found->second : no_records;
			const json::array& now_records = now_found != records.now.end() ? now_found->second : no_records;
			// An edit of a record the base has (the editor puts a changed value in the slot of the same field) is a record added
			// after it: the canonical form keeps the later one of a field. A record the target no longer has, with no later one for
			// its field, cannot be said by a layer.
			const auto same_field = [] (const json::value& one, const json::value& other) {
				const json::object& a = one.as_object();
				const json::object& b = other.as_object();
				for (const std::string_view key : { "target", "op", "component", "path" }) {
					const auto* left = a.if_contains(key);
					const auto* right = b.if_contains(key);
					if ((left == nullptr) != (right == nullptr) || (left && *left != *right)) return false;
				}
				return true;
			};
			for (const auto& held : was_records) {
				const bool kept = std::ranges::find(now_records, held) != now_records.end()
					|| std::ranges::any_of(now_records, [&] (const json::value& later) { return same_field(held, later); });
				if (!kept) {
					why = std::format("{}: an override record of the base cannot be taken out in a layer", where);
					return false;
				}
			}
			json::array extra;
			for (const auto& record : now_records) {
				if (std::ranges::find(was_records, record) == was_records.end()) extra.push_back(record);
			}
			if (!extra.empty()) layer["overrides"] = std::move(extra);

			return layer_children(inherited, target, layer, why, where, records);
		}
	}

	std::optional<json::object> make_prefab_layer(desc::desc_system& descs, const prefab_desc& base, const prefab_desc& edited, std::string& why)
	{
		const auto stamped = [&] (const prefab_desc& prefab) {
			json::object object;
			prefab.serialize(object);
			descs.stamp_tree(object);
			return object;
		};
		const json::object inherited = stamped(base);
		const json::object target = stamped(edited);
		records_by_node was;
		records_by_node now;
		collect_records(base.get_root(), was);
		collect_records(edited.get_root(), now);
		const records_pair records{ was, now };

		const auto* was_id = inherited.if_contains("__id");
		const auto* now_id = target.if_contains("__id");
		if ((was_id == nullptr) != (now_id == nullptr) || (was_id && *was_id != *now_id)) {
			why = "the root of the prefab has another '__id' than its base";
			return std::nullopt;
		}
		json::object layer;
		if (!layer_of_node(inherited, target, layer, why, "", records)) return std::nullopt;
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
		const auto* base = dynamic_cast<const prefab_desc*>(parent.get());
		if (!base) return std::format("the base '{}' is not a prefab", parent_tag.view());
		const json::object target = stamped(body);

		std::string why;
		auto layer = make_prefab_layer(descs, *base, prefab, why);
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
