#include <snk/scene/level/scn_override_record.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_node_id.h>
#include <snk/common/ds/ds_json_storage.hpp>
#include <snk/desc/desc_system.h>
#include <algorithm>
#include <format>
#include <limits>
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

		std::vector<std::string> pointer_tokens(std::string_view pointer)
		{
			std::vector<std::string> tokens;
			size_t begin = 1;
			while (begin <= pointer.size()) {
				size_t end = pointer.find('/', begin);
				if (end == std::string_view::npos) end = pointer.size();
				std::string token;
				for (size_t i = begin; i < end; ++i) {
					if (pointer[i] == '~' && i + 1 < end && (pointer[i + 1] == '0' || pointer[i + 1] == '1')) {
						token += pointer[i + 1] == '0' ? '~' : '/';
						++i;
					} else {
						token += pointer[i];
					}
				}
				tokens.push_back(std::move(token));
				begin = end + 1;
			}
			return tokens;
		}

		std::string pointer_of(const std::vector<std::string>& tokens)
		{
			std::string pointer;
			for (const std::string& token : tokens) {
				pointer += '/';
				for (char c : token) {
					if (c == '~') pointer += "~0";
					else if (c == '/') pointer += "~1";
					else pointer += c;
				}
			}
			return pointer;
		}

		bool is_index_token(std::string_view token)
		{
			return token == "-" || (!token.empty() && std::ranges::all_of(token, [] (char c) { return c >= '0' && c <= '9'; }));
		}

		bool is_header_key(std::string_view key)
		{
			return key == "__type" || key == "__version";
		}

		// Where `after` (the sparse object with the value, migrated) differs from `bare` (the same without it): each place
		// is a pointer and the value there. An object present on both sides is compared member by member, so a step that
		// fills a default next to the value does not count.
		void collect_differences(const boost::json::object& after, const boost::json::object* bare, std::vector<std::string>& at,
			std::vector<std::pair<std::vector<std::string>, boost::json::value>>& out, bool root)
		{
			for (const auto& [key, value] : after) {
				if (root && is_header_key(key)) continue;
				at.emplace_back(key);
				const boost::json::value* other = bare ? bare->if_contains(key) : nullptr;
				if (!other) {
					out.emplace_back(at, value);
				} else if (value.is_object() && other->is_object()) {
					collect_differences(value.as_object(), &other->as_object(), at, out, false);
				} else if (value != *other) {
					out.emplace_back(at, value);
				}
				at.pop_back();
			}
		}

		boost::json::object sparse_object(std::string_view type, int version, const std::vector<std::string>& tokens,
			const boost::json::value* value)
		{
			boost::json::object root;
			root["__type"] = type;
			root["__version"] = version;
			boost::json::object* holder = &root;
			for (size_t i = 0; i + 1 < tokens.size(); ++i) {
				holder = &(*holder)[tokens[i]].emplace_object();
			}
			if (value && !tokens.empty()) {
				(*holder)[tokens.back()] = *value;
			}
			return root;
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
			ds::keep_in_default_resource(record.value);
		}
		if (const auto* version = obj.if_contains("version")) {
			const auto number = version->is_int64() ? version->as_int64() : (version->is_uint64() ? static_cast<int64_t>(version->as_uint64()) : 0);
			if (number < 1 || number > std::numeric_limits<int>::max()) {
				egLOG_WARN("desc", "override record: 'version' must be a positive integer, read as not known");
			} else {
				record.type_version = static_cast<int>(number);
			}
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
		if (record.op == override_op::set && record.type_version > 0) {
			out["version"] = record.type_version;
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

	namespace {

		using tokens_t = std::vector<std::string>;

		void find_marker(const boost::json::value& value, const boost::json::value& marker, tokens_t& at, std::vector<tokens_t>& found)
		{
			if (value == marker) {
				found.push_back(at);
			} else if (value.is_object()) {
				for (const auto& [key, member] : value.as_object()) {
					at.emplace_back(key);
					find_marker(member, marker, at, found);
					at.pop_back();
				}
			}
		}

		const boost::json::value* value_at(const boost::json::object& root, const tokens_t& tokens)
		{
			const boost::json::value* value = nullptr;
			const boost::json::object* holder = &root;
			for (const std::string& token : tokens) {
				if (!holder) return nullptr;
				value = holder->if_contains(token);
				if (!value) return nullptr;
				holder = value->is_object() ? &value->as_object() : nullptr;
			}
			return value;
		}

		struct located {
			record_migration outcome = record_migration::kept;
			tokens_t tokens;
			boost::json::value value;
			std::string why;
		};

		// Where the steps of `type` from `version` take a value written at `tokens` (no array index among them), and what
		// they make of it. A marker in place of the value finds the place even when a step fills that field with a default
		// equal to the value; a step that converts the value (the marker then disappears or the step refuses it) is found
		// by what differs between the sparse object with the value and the one without.
		located locate(const desc::desc_system& descs, std::string_view type, int version, const tokens_t& tokens,
			const boost::json::value& value, std::string_view named)
		{
			static const boost::json::value marker = boost::json::string("\x01snk:override-record-marker");
			std::vector<desc::migration_warning> ignored;
			std::vector<tokens_t> marked;
			boost::json::object with_marker = sparse_object(type, version, tokens, &marker);
			try {
				descs.migrate_tree(with_marker, named, ignored);
				tokens_t at;
				find_marker(with_marker, marker, at, marked);
			} catch (const std::exception&) {
				marked.clear();
			}
			if (marked.size() > 1) {
				return { record_migration::duplicated, {}, {}, std::format("the steps put it in {} places", marked.size()) };
			}

			boost::json::object with_value = sparse_object(type, version, tokens, &value);
			boost::json::object bare = sparse_object(type, version, tokens, nullptr);
			try {
				descs.migrate_tree(with_value, named, ignored);
				descs.migrate_tree(bare, named, ignored);
			} catch (const std::exception& error) {
				return { record_migration::failed, {}, {}, std::format("the steps refused it: {}", error.what()) };
			}
			std::vector<std::pair<tokens_t, boost::json::value>> differences;
			tokens_t at;
			collect_differences(with_value, &bare, at, differences, true);
			// A step that wraps the value in an object carries the marker deeper than the field: the field is the one difference.
			const bool wrapped = marked.size() == 1 && differences.size() == 1
				&& differences.front().first.size() < marked.front().size()
				&& std::equal(differences.front().first.begin(), differences.front().first.end(), marked.front().begin());
			if (marked.size() == 1 && !wrapped) {
				const boost::json::value* converted = value_at(with_value, marked.front());
				return { record_migration::kept, marked.front(), converted ? *converted : value, {} };
			}
			if (differences.empty()) {
				return { record_migration::dropped, {}, {}, "a step removed the field" };
			}
			if (differences.size() > 1) {
				return { record_migration::duplicated, {}, {}, std::format("the steps put it in {} places", differences.size()) };
			}
			return { record_migration::kept, std::move(differences.front().first), std::move(differences.front().second), {} };
		}

		// The leaves of a merge into a whole component: each member down to a value that is not a plain object (a typed
		// object is a leaf, the steps of its own type carry it).
		void merge_leaves(const boost::json::object& object, tokens_t& at, std::vector<std::pair<tokens_t, const boost::json::value*>>& out)
		{
			for (const auto& [key, member] : object) {
				at.emplace_back(key);
				if (member.is_object() && !member.as_object().contains("__type") && !member.as_object().empty()) {
					merge_leaves(member.as_object(), at, out);
				} else {
					out.emplace_back(at, &member);
				}
				at.pop_back();
			}
		}

		void put_at(boost::json::object& root, const tokens_t& tokens, boost::json::value value)
		{
			boost::json::object* holder = &root;
			for (size_t i = 0; i + 1 < tokens.size(); ++i) {
				boost::json::value& next = (*holder)[tokens[i]];
				holder = next.is_object() ? &next.as_object() : &next.emplace_object();
			}
			(*holder)[tokens.back()] = std::move(value);
		}
	}

	record_migration_result migrate_override_record(const desc::desc_system& descs, std::string_view type, override_record& record)
	{
		if (record.op != override_op::set || type.empty() || record.component == transform_pseudo_component) {
			return {};
		}
		const int current = descs.current_version(type);
		if (record.type_version <= 0 || record.type_version >= current) {
			// Newer than the engine knows is left as written: the off-the-fields warning names it when it is applied.
			if (record.type_version <= 0) record.type_version = current;
			return {};
		}
		const std::string named = std::format("override of '{}' at '{}' (version {} of '{}')", record.component, record.path,
			record.type_version, type);
		auto refused = [&](const located& where) {
			return record_migration_result{ where.outcome, std::format("{}: {}", named, where.why) };
		};

		tokens_t tokens = pointer_tokens(record.path);
		if (tokens.empty()) {
			// A merge into the whole component: each field it sets goes its own way, the ones a step removed fall out.
			if (!record.value.is_object()) {
				return { record_migration::failed, named + ": a merge into a component is an object" };
			}
			std::vector<std::pair<tokens_t, const boost::json::value*>> leaves;
			tokens_t at;
			merge_leaves(record.value.as_object(), at, leaves);
			boost::json::object merged;
			for (const auto& [leaf, value] : leaves) {
				located where = locate(descs, type, record.type_version, leaf, *value, named);
				if (where.outcome == record_migration::dropped) continue;
				if (where.outcome != record_migration::kept) return refused(where);
				put_at(merged, where.tokens, std::move(where.value));
			}
			if (merged.empty() && !leaves.empty()) {
				return { record_migration::dropped, named + ": the steps removed every field it sets" };
			}
			const bool changed = merged != record.value.as_object();
			if (changed) {
				record.value = std::move(merged);
				ds::keep_in_default_resource(record.value);
			}
			record.type_version = current;
			return { changed ? record_migration::moved : record_migration::kept, {} };
		}

		// A path through an array is followed only up to the array: the steps move the field that holds it, what is inside
		// is not known here, so the field must come out as it went in.
		const auto index = std::ranges::find_if(tokens, is_index_token);
		const tokens_t rest(index, tokens.end());
		tokens.erase(index, tokens.end());
		if (tokens.empty()) {
			return { record_migration::failed, named + ": a component is not an array" };
		}
		static const boost::json::value placeholder = boost::json::array{};
		located where = locate(descs, type, record.type_version, tokens, rest.empty() ? record.value : placeholder, named);
		if (where.outcome != record_migration::kept) return refused(where);
		if (!rest.empty() && where.value != placeholder) {
			return { record_migration::failed, named + ": a step changed the array the path goes through" };
		}

		where.tokens.insert(where.tokens.end(), rest.begin(), rest.end());
		std::string new_path = pointer_of(where.tokens);
		const bool changed = new_path != record.path || (rest.empty() && where.value != record.value);
		record.path = std::move(new_path);
		if (rest.empty()) {
			record.value = std::move(where.value);
			ds::keep_in_default_resource(record.value);
		}
		record.type_version = current;
		return { changed ? record_migration::moved : record_migration::kept, {} };
	}
}
