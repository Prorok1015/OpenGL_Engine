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

		// A pointer whose writes can be collapsed or reordered safely: not the document root and no array index or
		// "-" (appending twice is two appends, and "/arr/10" sorts before "/arr/2").
		bool is_plain_pointer(const std::string& pointer)
		{
			if (pointer.empty() || pointer.front() != '/') {
				return false;
			}
			size_t begin = 1;
			while (begin <= pointer.size()) {
				size_t end = pointer.find('/', begin);
				if (end == std::string::npos) end = pointer.size();
				const std::string_view token = std::string_view{ pointer }.substr(begin, end - begin);
				const bool numeric = !token.empty() && std::ranges::all_of(token, [] (char c) { return c >= '0' && c <= '9'; });
				if (numeric || token == "-") {
					return false;
				}
				begin = end + 1;
			}
			return true;
		}

		bool is_plain_set(const override_record& record)
		{
			return record.op == override_op::set && is_plain_pointer(record.path);
		}

		bool related_targets(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
		{
			return is_prefix(a, b) || is_prefix(b, a);
		}

		// Can the record at `between` change what happens to `earlier`, so that dropping `earlier` could alter the result?
		bool is_barrier(const override_record& between, const override_record& earlier)
		{
			if (between.op != override_op::set) {
				return related_targets(between.target, earlier.target);
			}
			return between.target == earlier.target && between.component == earlier.component && !is_plain_pointer(between.path);
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
		// Only plain pointer writes are touched. Everything else (remove, add_node, merges at "", array pointers)
		// keeps its written place and is never dropped: add_node can merge into an existing node, so it neither
		// cancels against a remove nor replaces an earlier add_node.
		const size_t count = records.size();
		std::vector<bool> dead(count, false);
		for (size_t later = 0; later < count; ++later) {
			if (!is_plain_set(records[later])) continue;
			for (size_t earlier = 0; earlier < later; ++earlier) {
				const override_record& e = records[earlier];
				if (dead[earlier] || !is_plain_set(e) || e.target != records[later].target || e.component != records[later].component
					|| !pointer_covers(records[later].path, e.path)) {
					continue;
				}
				bool blocked = false;
				for (size_t between = earlier + 1; between < later && !blocked; ++between) {
					blocked = !dead[between] && is_barrier(records[between], e);
				}
				if (!blocked) dead[earlier] = true;
			}
		}

		std::vector<override_record> kept;
		kept.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			if (!dead[i]) kept.push_back(std::move(records[i]));
		}

		// Sorting only inside runs of plain pointer writes: those commute unless one pointer is a parent of the
		// other, and a parent sorts before its children.
		auto less = [] (const override_record& a, const override_record& b) {
			if (a.target != b.target) return a.target < b.target;
			if (a.component != b.component) return a.component < b.component;
			return a.path < b.path;
		};
		for (size_t begin = 0; begin < kept.size();) {
			if (!is_plain_set(kept[begin])) {
				++begin;
				continue;
			}
			size_t end = begin;
			while (end < kept.size() && is_plain_set(kept[end])) ++end;
			std::stable_sort(kept.begin() + begin, kept.begin() + end, less);
			begin = end;
		}
		records = std::move(kept);
	}
}
