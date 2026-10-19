#include <snk/command/cmd_json_schema.h>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <format>
#include <string_view>

namespace json = boost::json;

namespace {
	constexpr std::array<std::string_view, 7> TYPE_NAMES = { "integer", "number", "string", "boolean", "object", "array", "null" };

	constexpr std::array<std::string_view, 12> KEYWORDS = {
		"type", "properties", "required", "additionalProperties", "enum", "minimum", "maximum", "minLength", "minProperties", "items",
		"description", "title" };

	std::size_t char_count(std::string_view utf8)
	{
		std::size_t count = 0;
		for (const char c : utf8) {
			if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
		}
		return count;
	}

	bool is_type_name(std::string_view name)
	{
		return std::find(TYPE_NAMES.begin(), TYPE_NAMES.end(), name) != TYPE_NAMES.end();
	}

	bool matches_type(std::string_view type, const json::value& v)
	{
		if (type == "integer") return v.is_int64() || v.is_uint64() || (v.is_double() && std::floor(v.as_double()) == v.as_double());
		if (type == "number") return v.is_number();
		if (type == "string") return v.is_string();
		if (type == "boolean") return v.is_bool();
		if (type == "object") return v.is_object();
		if (type == "array") return v.is_array();
		return v.is_null();
	}

	double number_of(const json::value& v)
	{
		if (v.is_int64()) return static_cast<double>(v.as_int64());
		if (v.is_uint64()) return static_cast<double>(v.as_uint64());
		return v.as_double();
	}

	// True if the schema's "type" is `name` or a list holding it. A keyword that only means something for one type is
	// refused on any other, so a schema never validates less than it says.
	bool declares_type(const json::object& obj, std::string_view name)
	{
		const json::value* type = obj.if_contains("type");
		if (!type) return false;
		if (type->is_string()) return type->as_string() == name;
		if (type->is_array()) {
			for (const auto& entry : type->as_array()) {
				if (entry.is_string() && entry.as_string() == name) return true;
			}
		}
		return false;
	}

	std::optional<std::string> check_node(const json::value& schema, const std::string& path)
	{
		if (!schema.is_object()) return std::format("{}: a schema must be an object", path);
		const json::object& obj = schema.as_object();

		for (const auto& [keyword, unused] : obj) {
			const std::string_view name(keyword);
			if (std::find(KEYWORDS.begin(), KEYWORDS.end(), name) == KEYWORDS.end())
				return std::format("{}: unsupported keyword '{}'", path, name);
		}
		if (const json::value* length = obj.if_contains("minLength"); length && (!length->is_int64() || length->as_int64() < 0))
			return std::format("{}: minLength must be a non-negative integer", path);

		if (const json::value* type = obj.if_contains("type")) {
			const auto is_bad = [](const json::value& name) {
				return !name.is_string() || !is_type_name(name.as_string());
			};
			if (type->is_array()) {
				if (type->as_array().empty()) return std::format("{}: type list is empty", path);
				for (const auto& name : type->as_array()) {
					if (is_bad(name)) return std::format("{}: unknown type", path);
				}
			} else if (is_bad(*type)) {
				return std::format("{}: unknown type", path);
			}
		}
		const json::value* props = obj.if_contains("properties");
		if (props && !props->is_object()) return std::format("{}: properties must be an object", path);
		if (const json::value* required = obj.if_contains("required")) {
			if (!required->is_array()) return std::format("{}: required must be an array", path);
			for (const auto& name : required->as_array()) {
				if (!name.is_string()) return std::format("{}: required holds a non-string", path);
				if (!props || !props->as_object().contains(name.as_string()))
					return std::format("{}: required '{}' is not in properties", path, std::string_view(name.as_string()));
			}
		}
		if (props) {
			for (const auto& [name, sub] : props->as_object()) {
				if (auto err = check_node(sub, std::format("{}.{}", path, std::string_view(name)))) return err;
			}
		}
		if (const json::value* count = obj.if_contains("minProperties")) {
			if (!count->is_int64() || count->as_int64() < 0) return std::format("{}: minProperties must be a non-negative integer", path);
			if (!declares_type(obj, "object")) return std::format("{}: minProperties needs type object", path);
		}
		if (const json::value* items = obj.if_contains("items")) {
			if (!declares_type(obj, "array")) return std::format("{}: items needs type array", path);
			if (auto err = check_node(*items, path + "[]")) return err;
		}
		if (const json::value* extra = obj.if_contains("additionalProperties"); extra && !extra->is_bool())
			return std::format("{}: additionalProperties must be a boolean", path);
		if (const json::value* values = obj.if_contains("enum"); values && (!values->is_array() || values->as_array().empty()))
			return std::format("{}: enum must be a non-empty array", path);
		for (const char* key : { "minimum", "maximum" }) {
			if (const json::value* bound = obj.if_contains(key); bound && !bound->is_number())
				return std::format("{}: {} must be a number", path, key);
		}
		return std::nullopt;
	}

	std::optional<std::string> validate_node(const json::object& schema, const json::value& v, const std::string& path)
	{
		if (const json::value* type = schema.if_contains("type")) {
			bool ok = false;
			if (type->is_array()) {
				for (const auto& name : type->as_array()) ok = ok || matches_type(name.as_string(), v);
			} else {
				ok = matches_type(type->as_string(), v);
			}
			if (!ok) return std::format("{}: expected {}", path, json::serialize(*type));
		}
		if (const json::value* values = schema.if_contains("enum")) {
			const auto& list = values->as_array();
			if (std::find(list.begin(), list.end(), v) == list.end())
				return std::format("{}: not one of {}", path, json::serialize(*values));
		}
		if (v.is_string()) {
			if (const json::value* min = schema.if_contains("minLength"); min && char_count(v.as_string()) < static_cast<std::size_t>(min->as_int64()))
				return std::format("{}: shorter than {} characters", path, min->as_int64());
		}
		if (v.is_number()) {
			const double number = number_of(v);
			if (const json::value* lo = schema.if_contains("minimum"); lo && number < number_of(*lo))
				return std::format("{}: {} is below the minimum {}", path, json::serialize(v), json::serialize(*lo));
			if (const json::value* hi = schema.if_contains("maximum"); hi && number > number_of(*hi))
				return std::format("{}: {} is above the maximum {}", path, json::serialize(v), json::serialize(*hi));
		}
		if (v.is_array()) {
			if (const json::value* items = schema.if_contains("items")) {
				std::size_t index = 0;
				for (const auto& element : v.as_array()) {
					if (auto err = validate_node(items->as_object(), element, std::format("{}[{}]", path, index))) return err;
					++index;
				}
			}
		}
		if (v.is_object()) {
			const json::object& obj = v.as_object();
			if (const json::value* min = schema.if_contains("minProperties"); min && obj.size() < static_cast<std::size_t>(min->as_int64()))
				return std::format("{}: needs at least {} properties", path, min->as_int64());
			const json::value* props = schema.if_contains("properties");
			if (const json::value* required = schema.if_contains("required")) {
				for (const auto& name : required->as_array()) {
					if (!obj.contains(name.as_string()))
						return std::format("{}: missing required '{}'", path, std::string_view(name.as_string()));
				}
			}
			for (const auto& [name, value] : obj) {
				const json::value* sub = props ? props->as_object().if_contains(name) : nullptr;
				if (sub) {
					if (auto err = validate_node(sub->as_object(), value, std::format("{}.{}", path, std::string_view(name)))) return err;
				} else if (const json::value* extra = schema.if_contains("additionalProperties"); extra && !extra->as_bool()) {
					return std::format("{}: unexpected property '{}'", path, std::string_view(name));
				}
			}
		}
		return std::nullopt;
	}
}

std::optional<std::int64_t> cmd::json_schema::integer_of(const json::value& value)
{
	if (value.is_int64()) return value.get_int64();
	if (value.is_uint64()) {
		const std::uint64_t big = value.get_uint64();
		if (big > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
		return static_cast<std::int64_t>(big);
	}
	if (value.is_double()) {
		const double d = value.get_double();
		// 2^63 is exactly representable; anything from it up (or NaN) does not fit.
		if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0) || std::floor(d) != d) return std::nullopt;
		return static_cast<std::int64_t>(d);
	}
	return std::nullopt;
}

std::optional<std::string> cmd::json_schema::check_schema(const json::value& schema)
{
	return check_node(schema, "$");
}

std::optional<std::string> cmd::json_schema::validate(const json::value& schema, const json::value& instance)
{
	if (!schema.is_object()) return std::string("$: a schema must be an object");
	return validate_node(schema.as_object(), instance, "$");
}
