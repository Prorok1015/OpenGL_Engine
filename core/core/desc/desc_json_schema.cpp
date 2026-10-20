#include <snk/desc/desc_json_schema.h>
#include <boost/json/array.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	// Nested structures are bounded so a cyclic registry cannot recurse forever.
	constexpr int max_depth = 8;

	std::size_t components_of(desc::field_kind kind)
	{
		switch (kind) {
		case desc::field_kind::vec2: return 2;
		case desc::field_kind::vec3: return 3;
		case desc::field_kind::vec4:
		case desc::field_kind::quat:
		case desc::field_kind::color: return 4;
		default: return 0;
		}
	}

	boost::json::object typed(std::string_view type)
	{
		return boost::json::object{ { "type", boost::json::string(type) } };
	}

	boost::json::object object_schema(const desc::type_schema& schema, const desc::schema_registry& resolve, int depth);

	boost::json::object value_schema(desc::field_kind kind, const std::string& ref_type, const desc::schema_registry& resolve, int depth)
	{
		using desc::field_kind;
		switch (kind) {
		case field_kind::boolean: return typed("boolean");
		case field_kind::i32:
		case field_kind::i64: return typed("integer");
		case field_kind::u32:
		case field_kind::u64:
		case field_kind::entity: {
			auto out = typed("integer");
			out["minimum"] = 0;
			return out;
		}
		case field_kind::f32:
		case field_kind::f64: return typed("number");
		case field_kind::vec2:
		case field_kind::vec3:
		case field_kind::vec4:
		case field_kind::quat:
		case field_kind::color: {
			auto out = typed("array");
			out["items"] = typed("number");
			out["minItems"] = components_of(kind);
			out["maxItems"] = components_of(kind);
			return out;
		}
		case field_kind::string:
		case field_kind::res_tag: return typed("string");
		case field_kind::node_ref: {
			auto out = typed("string");
			out["pattern"] = "^[0-9a-f]{16}$";
			return out;
		}
		case field_kind::enumeration: {
			const desc::enum_schema* items = resolve.find_enum(ref_type);
			if (!items) {
				return typed("string");
			}
			boost::json::array names;
			for (const auto& item : items->items) {
				names.emplace_back(boost::json::string(item.name));
			}
			return boost::json::object{ { "enum", std::move(names) } };
		}
		case field_kind::desc_ref: {
			boost::json::array types;
			types.emplace_back("string");
			types.emplace_back("object");
			boost::json::object out;
			out["type"] = std::move(types);
			out["description"] = "a tag of a desc, or an inline desc object with a string __type";
			return out;
		}
		case field_kind::structure: {
			const desc::type_schema* nested = resolve.find(ref_type);
			if (nested && depth < max_depth) {
				return object_schema(*nested, resolve, depth + 1);
			}
			return typed("object");
		}
		case field_kind::array:
		case field_kind::custom: break;
		}
		return boost::json::object{};
	}

	boost::json::object field_schema_of(const desc::field_schema& field, const desc::schema_registry& resolve, int depth)
	{
		boost::json::object out;
		if (field.kind == desc::field_kind::array) {
			out = typed("array");
			out["items"] = value_schema(field.element, field.ref_type, resolve, depth);
		} else {
			out = value_schema(field.kind, field.ref_type, resolve, depth);
		}
		if (field.range) {
			// A vector or colour keeps the bounds on its components.
			boost::json::object* bounded = &out;
			if (components_of(field.kind) != 0) {
				bounded = &out["items"].as_object();
			}
			(*bounded)["minimum"] = field.range->min;
			(*bounded)["maximum"] = field.range->max;
		}
		if (!field.default_value.is_null()) {
			out["default"] = field.default_value;
		}
		if (!field.tooltip.empty()) {
			out["description"] = boost::json::string(field.tooltip);
		}
		if (field.read_only || field.write == desc::write_policy::never) {
			out["readOnly"] = true;
		}
		if (field.degrees) {
			out["x-degrees"] = true;
		}
		if (field.write == desc::write_policy::null_if_empty) {
			boost::json::array either;
			either.emplace_back(std::move(out));
			either.emplace_back(typed("null"));
			boost::json::object wrapped;
			wrapped["anyOf"] = std::move(either);
			return wrapped;
		}
		return out;
	}

	std::vector<std::string> pointer_tokens(std::string_view pointer)
	{
		std::vector<std::string> tokens;
		std::size_t begin = 0;
		while (begin < pointer.size()) {
			if (pointer[begin] == '/') {
				++begin;
			}
			const std::size_t end = pointer.find('/', begin);
			std::string token{ pointer.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin) };
			for (std::size_t at = token.find('~'); at != std::string::npos && at + 1 < token.size(); at = token.find('~', at + 1)) {
				if (token[at + 1] == '1') {
					token.replace(at, 2, "/");
				} else if (token[at + 1] == '0') {
					token.replace(at, 2, "~");
				}
			}
			tokens.push_back(std::move(token));
			if (end == std::string_view::npos) {
				break;
			}
			begin = end;
		}
		return tokens;
	}

	void place(boost::json::object& properties, const std::vector<std::string>& tokens, std::size_t index, boost::json::object leaf)
	{
		const std::string& token = tokens[index];
		if (index + 1 == tokens.size()) {
			properties.insert_or_assign(token, std::move(leaf));
			return;
		}
		auto it = properties.find(token);
		if (it == properties.end() || !it->value().is_object() || !it->value().as_object().contains("properties")) {
			boost::json::object group;
			group["type"] = "object";
			group["properties"] = boost::json::object{};
			properties.insert_or_assign(token, std::move(group));
			it = properties.find(token);
		}
		place(it->value().as_object()["properties"].as_object(), tokens, index + 1, std::move(leaf));
	}

	boost::json::object object_schema(const desc::type_schema& schema, const desc::schema_registry& resolve, int depth)
	{
		boost::json::object properties;
		for (const auto& field : schema.fields) {
			const std::vector<std::string> tokens = pointer_tokens(field.key);
			if (tokens.empty() || tokens.front().empty()) {
				continue;
			}
			place(properties, tokens, 0, field_schema_of(field, resolve, depth));
		}
		boost::json::object out;
		out["type"] = "object";
		out["properties"] = std::move(properties);
		return out;
	}
}

boost::json::object desc::to_json_schema(const type_schema& schema, const schema_registry& resolve)
{
	boost::json::object out = object_schema(schema, resolve, 0);
	out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
	out["title"] = boost::json::string(schema.name);
	out["x-version"] = schema.version;
	out["x-kind"] = boost::json::string(to_string(schema.kind));
	boost::json::object type_marker;
	type_marker["const"] = boost::json::string(schema.name);
	out["properties"].as_object().insert_or_assign("__type", std::move(type_marker));
	return out;
}
