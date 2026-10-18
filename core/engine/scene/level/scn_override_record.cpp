#include "scn_override_record.h"
#include "logger/engine_log.h"
#include "desc_node_id.h"
#include <algorithm>
#include <map>

namespace scn {

	namespace {

		bool is_prefix(const std::vector<uint64_t>& prefix, const std::vector<uint64_t>& of)
		{
			return prefix.size() <= of.size() && std::equal(prefix.begin(), prefix.end(), of.begin());
		}

		// `outer` is the same pointer or a parent of `inner`. The empty pointer is excluded: a record at "" merges
		// into the component instead of replacing it, so it hides nothing.
		bool pointer_covers(const std::string& outer, const std::string& inner)
		{
			if (outer.empty()) {
				return false;
			}
			return inner == outer || (inner.size() > outer.size() && inner.starts_with(outer) && inner[outer.size()] == '/');
		}

		uint64_t added_node_id(const override_record& record)
		{
			if (!record.value.is_object()) {
				return desc::invalid_node_id;
			}
			const auto* id = record.value.as_object().if_contains("__id");
			if (!id || !id->is_string()) {
				return desc::invalid_node_id;
			}
			return desc::parse_node_id(id->as_string()).value_or(desc::invalid_node_id);
		}

		// Is `earlier` made irrelevant by the later record `later`? `later_is_redundant` is set when `later`
		// only undoes an addition made by `earlier`.
		bool absorbs(const override_record& later, const override_record& earlier, bool& later_is_redundant)
		{
			switch (later.op) {
			case override_op::set:
				return earlier.op == override_op::set && earlier.target == later.target
					&& earlier.component == later.component && pointer_covers(later.path, earlier.path);

			case override_op::remove:
				if (!later.component.empty()) {
					return earlier.op != override_op::add_node && earlier.target == later.target && earlier.component == later.component;
				}
				if (later.target.empty()) {
					return false;
				}
				if (earlier.op == override_op::add_node) {
					if (is_prefix(later.target, earlier.target)) {
						return true;
					}
					if (earlier.target.size() + 1 == later.target.size() && is_prefix(earlier.target, later.target)
						&& added_node_id(earlier) == later.target.back()) {
						later_is_redundant = true;
						return true;
					}
					return false;
				}
				return is_prefix(later.target, earlier.target);

			case override_op::add_node:
				return earlier.op == override_op::add_node && earlier.target == later.target
					&& added_node_id(later) != desc::invalid_node_id && added_node_id(later) == added_node_id(earlier);
			}
			return false;
		}

		std::string_view op_name(override_op op)
		{
			switch (op) {
			case override_op::set: return "set";
			case override_op::remove: return "remove";
			case override_op::add_node: return "add_node";
			}
			return "set";
		}
	}

	std::optional<override_record> parse_override_record(const boost::json::value& json)
	{
		if (!json.is_object()) {
			egLOG_WARN("desc", "override record is not an object, skipping");
			return std::nullopt;
		}
		const auto& obj = json.as_object();
		override_record record;

		if (const auto* op = obj.if_contains("op")) {
			if (!op->is_string()) {
				egLOG_WARN("desc", "override record: 'op' must be a string, skipping");
				return std::nullopt;
			}
			if (op->as_string() == "set") record.op = override_op::set;
			else if (op->as_string() == "remove") record.op = override_op::remove;
			else if (op->as_string() == "add_node") record.op = override_op::add_node;
			else {
				egLOG_WARN("desc", "override record: unknown op '{}', skipping", std::string_view{ op->as_string() });
				return std::nullopt;
			}
		}

		if (const auto* target = obj.if_contains("target")) {
			if (!target->is_array()) {
				egLOG_WARN("desc", "override record: 'target' must be an array of node ids, skipping");
				return std::nullopt;
			}
			for (const auto& element : target->as_array()) {
				auto id = element.is_string() ? desc::parse_node_id(element.as_string()) : std::nullopt;
				if (!id) {
					egLOG_WARN("desc", "override record: malformed node id in 'target', skipping");
					return std::nullopt;
				}
				record.target.push_back(*id);
			}
		}

		if (const auto* component = obj.if_contains("component"); component && component->is_string()) {
			record.component = std::string{ component->as_string() };
		}
		if (const auto* path = obj.if_contains("path"); path && path->is_string()) {
			record.path = std::string{ path->as_string() };
		}
		if (const auto* value = obj.if_contains("value")) {
			record.value = *value;
		}

		if (record.op == override_op::set && (record.component.empty() || !obj.contains("value"))) {
			egLOG_WARN("desc", "override record: 'set' needs a 'component' and a 'value', skipping");
			return std::nullopt;
		}
		if (record.op == override_op::add_node && !record.value.is_object()) {
			egLOG_WARN("desc", "override record: 'add_node' needs a node object as 'value', skipping");
			return std::nullopt;
		}
		return record;
	}

	boost::json::object override_record_to_json(const override_record& record)
	{
		boost::json::object out;
		boost::json::array target;
		target.reserve(record.target.size());
		for (uint64_t id : record.target) {
			target.emplace_back(desc::format_node_id(id));
		}
		out["target"] = std::move(target);
		if (record.op != override_op::set) {
			out["op"] = std::string{ op_name(record.op) };
		}
		if (!record.component.empty()) {
			out["component"] = record.component;
		}
		if (record.op == override_op::set || !record.path.empty()) {
			out["path"] = record.path;
		}
		if (record.op != override_op::remove) {
			out["value"] = record.value;
		}
		return out;
	}

	void canonicalize_overrides(std::vector<override_record>& records)
	{
		const size_t count = records.size();
		std::vector<bool> dead(count, false);
		for (size_t later = 0; later < count; ++later) {
			for (size_t earlier = 0; earlier < later; ++earlier) {
				if (dead[earlier]) continue;
				bool later_is_redundant = false;
				if (absorbs(records[later], records[earlier], later_is_redundant)) {
					dead[earlier] = true;
					if (later_is_redundant) dead[later] = true;
				}
			}
		}

		std::vector<override_record> kept;
		kept.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			if (!dead[i]) kept.push_back(std::move(records[i]));
		}

		// A merge at "" is order sensitive against the pointer records of its component: such a component keeps
		// its records in the order they were written.
		std::map<std::pair<std::vector<uint64_t>, std::string>, bool> has_merge;
		for (const auto& record : kept) {
			if (record.op == override_op::set && record.path.empty()) {
				has_merge[{ record.target, record.component }] = true;
			}
		}
		auto sort_path = [&] (const override_record& record) -> const std::string& {
			static const std::string empty;
			return has_merge.contains({ record.target, record.component }) ? empty : record.path;
		};
		std::ranges::stable_sort(kept, [&] (const override_record& a, const override_record& b) {
			if (a.target != b.target) return a.target < b.target;
			if (a.component != b.component) return a.component < b.component;
			return sort_path(a) < sort_path(b);
		});
		records = std::move(kept);
	}
}
