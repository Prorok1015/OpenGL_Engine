#include <snk/desc/desc_schema.h>
#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

namespace desc
{
	namespace
	{
		constexpr std::string_view field_kind_names[] = {
			"bool", "i32", "u32", "i64", "u64", "f32", "f64",
			"vec2", "vec3", "vec4", "quat", "color",
			"string", "enumeration", "res_tag", "desc_ref", "node_ref", "entity",
			"structure", "array", "custom",
		};
		constexpr std::string_view write_policy_names[] = { "always", "if_valid", "if_not_default" };
		constexpr std::string_view schema_kind_names[] = { "desc", "component", "struct" };

		template<typename E, std::size_t N>
		std::string_view name_of(const std::string_view (&names)[N], E value)
		{
			const auto index = static_cast<std::size_t>(value);
			return index < N ? names[index] : std::string_view{ "unknown" };
		}

		template<typename E, std::size_t N>
		std::optional<E> from_name(const std::string_view (&names)[N], std::string_view text)
		{
			for (std::size_t i = 0; i < N; ++i) {
				if (names[i] == text) {
					return static_cast<E>(i);
				}
			}
			return std::nullopt;
		}

		std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		[[noreturn]] void fail(const std::string& path, std::string_view what)
		{
			throw std::runtime_error(std::format("schema {}: {}", path, what));
		}

		void check_keys(const json::object& object, std::initializer_list<std::string_view> allowed, const std::string& path)
		{
			for (const auto& member : object) {
				if (member.key() == "migrations") {
					fail(path, "'migrations' is not supported yet (declarative migrations of runtime schemas come later)");
				}
				if (std::ranges::find(allowed, std::string_view{ member.key() }) == allowed.end()) {
					fail(path, std::format("unknown key '{}'", std::string_view{ member.key() }));
				}
			}
		}

		const json::value* find_value(const json::object& object, std::string_view key)
		{
			return object.if_contains(key);
		}

		std::string read_string(const json::object& object, std::string_view key, const std::string& path, bool required, std::string fallback = {})
		{
			const json::value* value = find_value(object, key);
			if (!value) {
				if (required) {
					fail(path, std::format("missing '{}'", key));
				}
				return fallback;
			}
			if (!value->is_string()) {
				fail(path, std::format("'{}' must be a string", key));
			}
			return std::string{ value->as_string() };
		}

		bool read_bool(const json::object& object, std::string_view key, const std::string& path, bool fallback)
		{
			const json::value* value = find_value(object, key);
			if (!value) {
				return fallback;
			}
			if (!value->is_bool()) {
				fail(path, std::format("'{}' must be a boolean", key));
			}
			return value->as_bool();
		}

		bool read_number(const json::value& value, double& out)
		{
			if (value.is_double()) {
				out = value.as_double();
			} else if (value.is_int64()) {
				out = static_cast<double>(value.as_int64());
			} else if (value.is_uint64()) {
				out = static_cast<double>(value.as_uint64());
			} else {
				return false;
			}
			return true;
		}

		json::value number_array(std::size_t count, double fill, bool float_precision)
		{
			json::array result;
			result.resize(count, json::value(float_precision ? static_cast<double>(static_cast<float>(fill)) : fill));
			return json::value(std::move(result));
		}

		std::size_t component_count(field_kind kind)
		{
			switch (kind) {
			case field_kind::vec2: return 2;
			case field_kind::vec3: return 3;
			case field_kind::vec4:
			case field_kind::quat:
			case field_kind::color: return 4;
			default: return 0;
			}
		}

		json::value zero_default(field_kind kind)
		{
			switch (kind) {
			case field_kind::boolean: return json::value(false);
			case field_kind::i32:
			case field_kind::i64: return json::value(std::int64_t{ 0 });
			case field_kind::u32:
			case field_kind::u64: return json::value(std::uint64_t{ 0 });
			case field_kind::f32:
			case field_kind::f64: return json::value(0.0);
			case field_kind::vec2:
			case field_kind::vec3:
			case field_kind::vec4:
			case field_kind::color: return number_array(component_count(kind), 0.0, true);
			case field_kind::quat: {
				json::array identity{ json::value(0.0), json::value(0.0), json::value(0.0), json::value(1.0) };
				return json::value(std::move(identity));
			}
			case field_kind::string:
			case field_kind::res_tag:
			case field_kind::node_ref: return json::value(std::string_view{});
			case field_kind::array: return json::value(json::array{});
			default: return json::value();
			}
		}

		// Brings a JSON default to the representation the C++ generator produces (value_to_json).
		json::value normalize_default(field_kind kind, field_kind element, const json::value& value, const std::string& path)
		{
			if (value.is_null()) {
				return value;
			}
			switch (kind) {
			case field_kind::boolean:
				if (!value.is_bool()) {
					fail(path, "default must be a boolean");
				}
				return value;
			case field_kind::i32:
			case field_kind::i64: {
				std::int64_t number = 0;
				if (value.is_int64()) {
					number = value.as_int64();
				} else if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
					number = static_cast<std::int64_t>(value.as_uint64());
				} else {
					fail(path, "default must be an integer");
				}
				if (kind == field_kind::i32 && (number < std::numeric_limits<std::int32_t>::min() || number > std::numeric_limits<std::int32_t>::max())) {
					fail(path, "default is out of the i32 range");
				}
				return json::value(number);
			}
			case field_kind::u32:
			case field_kind::u64: {
				std::uint64_t number = 0;
				if (value.is_uint64()) {
					number = value.as_uint64();
				} else if (value.is_int64() && value.as_int64() >= 0) {
					number = static_cast<std::uint64_t>(value.as_int64());
				} else {
					fail(path, "default must be a non-negative integer");
				}
				if (kind == field_kind::u32 && number > std::numeric_limits<std::uint32_t>::max()) {
					fail(path, "default is out of the u32 range");
				}
				return json::value(number);
			}
			case field_kind::f32:
			case field_kind::f64: {
				double number = 0.0;
				if (!read_number(value, number)) {
					fail(path, "default must be a number");
				}
				return json::value(kind == field_kind::f32 ? static_cast<double>(static_cast<float>(number)) : number);
			}
			case field_kind::vec2:
			case field_kind::vec3:
			case field_kind::vec4:
			case field_kind::quat:
			case field_kind::color: {
				const std::size_t count = component_count(kind);
				if (!value.is_array() || value.as_array().size() != count) {
					fail(path, std::format("default must be an array of {} numbers", count));
				}
				json::array result;
				result.reserve(count);
				for (const auto& component : value.as_array()) {
					double number = 0.0;
					if (!read_number(component, number)) {
						fail(path, "default must be an array of numbers");
					}
					result.push_back(json::value(static_cast<double>(static_cast<float>(number))));
				}
				return json::value(std::move(result));
			}
			case field_kind::string:
			case field_kind::enumeration:
			case field_kind::res_tag:
			case field_kind::node_ref:
				if (!value.is_string()) {
					fail(path, "default must be a string");
				}
				return value;
			case field_kind::array: {
				if (!value.is_array()) {
					fail(path, "default must be an array");
				}
				json::array result;
				result.reserve(value.as_array().size());
				for (const auto& item : value.as_array()) {
					result.push_back(normalize_default(element, field_kind::boolean, item, path));
				}
				return json::value(std::move(result));
			}
			default:
				return value;
			}
		}

		field_schema parse_field(const json::value& raw, const std::string& path)
		{
			if (!raw.is_object()) {
				fail(path, "must be an object");
			}
			const json::object& object = raw.as_object();
			check_keys(object, { "name", "key", "kind", "element", "ref_type", "default", "range", "write", "read_aliases",
				"optional", "editor_hidden", "read_only", "color", "degrees", "tooltip", "filter" }, path);

			field_schema field;
			field.name = read_string(object, "name", path, true);
			if (field.name.empty()) {
				fail(path, "'name' must not be empty");
			}
			const std::string where = std::format("{} '{}'", path, field.name);

			field.key = read_string(object, "key", where, false, "/" + field.name);
			if (field.key.empty() || field.key.front() != '/') {
				fail(where, "'key' must be a JSON pointer starting with '/'");
			}

			const std::string kind_text = read_string(object, "kind", where, true);
			const auto kind = field_kind_from_string(kind_text);
			if (!kind) {
				fail(where, std::format("unknown kind '{}'", kind_text));
			}
			field.kind = *kind;

			const json::value* element = find_value(object, "element");
			if (field.kind == field_kind::array) {
				if (!element || !element->is_string()) {
					fail(where, "an array field needs a string 'element' kind");
				}
				const auto element_kind = field_kind_from_string(element->as_string());
				if (!element_kind) {
					fail(where, std::format("unknown element kind '{}'", std::string_view{ element->as_string() }));
				}
				field.element = *element_kind;
			} else if (element) {
				fail(where, "'element' is only valid for array fields");
			}

			field.ref_type = read_string(object, "ref_type", where, false);
			field.tooltip = read_string(object, "tooltip", where, false);
			field.filter = read_string(object, "filter", where, false);
			field.optional = read_bool(object, "optional", where, false);
			field.editor_hidden = read_bool(object, "editor_hidden", where, false);
			field.read_only = read_bool(object, "read_only", where, false);
			field.color = read_bool(object, "color", where, false);
			field.degrees = read_bool(object, "degrees", where, false);

			if (const json::value* value = find_value(object, "default")) {
				field.default_value = normalize_default(field.kind, field.element, *value, where);
			} else {
				field.default_value = zero_default(field.kind);
			}

			if (const json::value* range = find_value(object, "range")) {
				if (!range->is_array() || range->as_array().size() < 2 || range->as_array().size() > 3) {
					fail(where, "'range' must be [min, max] or [min, max, step]");
				}
				field_range parsed;
				const auto& bounds = range->as_array();
				if (!read_number(bounds[0], parsed.min) || !read_number(bounds[1], parsed.max) || (bounds.size() == 3 && !read_number(bounds[2], parsed.step))) {
					fail(where, "'range' must hold numbers");
				}
				if (parsed.min > parsed.max) {
					fail(where, "'range' min is greater than max");
				}
				field.range = parsed;
			}

			if (const json::value* write = find_value(object, "write")) {
				const auto policy = write->is_string() ? write_policy_from_string(write->as_string()) : std::nullopt;
				if (!policy) {
					fail(where, "'write' must be always, if_valid or if_not_default");
				}
				field.write = *policy;
			}

			if (const json::value* aliases = find_value(object, "read_aliases")) {
				if (!aliases->is_array()) {
					fail(where, "'read_aliases' must be an array of JSON pointers");
				}
				for (const auto& alias : aliases->as_array()) {
					if (!alias.is_string() || alias.as_string().empty() || alias.as_string().front() != '/') {
						fail(where, "'read_aliases' must be an array of JSON pointers");
					}
					field.read_aliases.emplace_back(alias.as_string());
				}
			}
			return field;
		}
	}

	std::string_view to_string(field_kind kind) { return name_of(field_kind_names, kind); }
	std::string_view to_string(write_policy policy) { return name_of(write_policy_names, policy); }
	std::string_view to_string(schema_kind kind) { return name_of(schema_kind_names, kind); }

	std::optional<field_kind> field_kind_from_string(std::string_view text) { return from_name<field_kind>(field_kind_names, text); }
	std::optional<write_policy> write_policy_from_string(std::string_view text) { return from_name<write_policy>(write_policy_names, text); }
	std::optional<schema_kind> schema_kind_from_string(std::string_view text) { return from_name<schema_kind>(schema_kind_names, text); }

	const type_schema* schema_registry::add(type_schema schema)
	{
		if (m_types.contains(schema.name)) {
			return nullptr;
		}
		const std::uint64_t id = schema.id;
		if (m_names_by_id.contains(id)) {
			return nullptr;
		}
		const std::string name = schema.name;
		m_names_by_id.emplace(id, name);
		return &m_types.emplace(name, std::move(schema)).first->second;
	}

	bool schema_registry::remove(std::string_view name)
	{
		auto it = m_types.find(std::string{ name });
		if (it == m_types.end()) {
			return false;
		}
		m_names_by_id.erase(it->second.id);
		m_types.erase(it);
		return true;
	}

	bool schema_registry::set_version(std::string_view name, int version)
	{
		auto it = m_types.find(std::string{ name });
		if (it == m_types.end()) {
			return false;
		}
		it->second.version = version;
		return true;
	}

	const type_schema* schema_registry::find(std::string_view name) const
	{
		auto it = m_types.find(std::string{ name });
		return it == m_types.end() ? nullptr : &it->second;
	}

	const type_schema* schema_registry::find(std::uint64_t id) const
	{
		auto it = m_names_by_id.find(id);
		return it == m_names_by_id.end() ? nullptr : find(it->second);
	}

	bool schema_registry::contains(std::string_view name) const
	{
		return m_types.contains(std::string{ name });
	}

	const enum_schema* schema_registry::add_enum(enum_schema schema)
	{
		if (m_enums.contains(schema.name)) {
			return nullptr;
		}
		const std::string name = schema.name;
		return &m_enums.emplace(name, std::move(schema)).first->second;
	}

	bool schema_registry::remove_enum(std::string_view name)
	{
		return m_enums.erase(std::string{ name }) > 0;
	}

	const enum_schema* schema_registry::find_enum(std::string_view name) const
	{
		auto it = m_enums.find(std::string{ name });
		return it == m_enums.end() ? nullptr : &it->second;
	}

	std::vector<std::string> schema_registry::type_names() const
	{
		std::vector<std::string> names;
		names.reserve(m_types.size());
		for (const auto& entry : m_types) {
			names.push_back(entry.first);
		}
		std::ranges::sort(names);
		return names;
	}

	blob_layout layout_of(field_kind kind)
	{
		switch (kind) {
		case field_kind::boolean: return { 1, 1 };
		case field_kind::i64:
		case field_kind::u64:
		case field_kind::f64:
		case field_kind::node_ref: return { 8, 8 };
		case field_kind::vec2: return { 8, 4 };
		case field_kind::vec3: return { 12, 4 };
		case field_kind::vec4:
		case field_kind::quat:
		case field_kind::color: return { 16, 4 };
		case field_kind::structure: return { 0, 1 };
		default: return { 4, 4 };
		}
	}

	void compute_layout(type_schema& schema, const schema_registry* resolve)
	{
		std::uint32_t offset = 0;
		std::uint32_t alignment = 1;
		for (auto& field : schema.fields) {
			blob_layout layout = layout_of(field.kind);
			if (field.kind == field_kind::structure) {
				const type_schema* nested = resolve ? resolve->find(field.ref_type) : nullptr;
				if (!nested) {
					throw std::runtime_error(std::format("schema '{}': structure field '{}' references unknown schema '{}'", schema.name, field.name, field.ref_type));
				}
				layout = nested->layout;
			}
			offset = align_up(offset, layout.alignment);
			field.offset = offset;
			offset += layout.size;
			alignment = std::max(alignment, layout.alignment);
		}
		schema.layout = blob_layout{ align_up(offset, alignment), alignment };
	}

	type_schema parse_type_schema(const json::object& object, const schema_registry* resolve)
	{
		check_keys(object, { "name", "version", "kind", "is_component", "is_document", "fields" }, "type");

		type_schema schema;
		schema.name = read_string(object, "name", "type", true);
		if (schema.name.empty()) {
			fail("type", "'name' must not be empty");
		}
		const std::string path = std::format("'{}'", schema.name);
		schema.id = schema_id(schema.name);

		if (const json::value* version = find_value(object, "version")) {
			if (!version->is_int64() || version->as_int64() < 1 || version->as_int64() > std::numeric_limits<int>::max()) {
				fail(path, "'version' must be a positive integer");
			}
			schema.version = static_cast<int>(version->as_int64());
		}

		const std::string kind_text = read_string(object, "kind", path, false, "desc");
		const auto kind = schema_kind_from_string(kind_text);
		if (!kind) {
			fail(path, std::format("unknown kind '{}'", kind_text));
		}
		schema.kind = *kind;
		schema.is_component = read_bool(object, "is_component", path, schema.kind == schema_kind::component);
		schema.is_document = read_bool(object, "is_document", path, false);

		const json::value* fields = find_value(object, "fields");
		if (!fields || !fields->is_array()) {
			fail(path, "'fields' must be an array");
		}
		std::unordered_set<std::string> names;
		std::unordered_set<std::string> keys;
		schema.fields.reserve(fields->as_array().size());
		std::size_t index = 0;
		for (const auto& raw : fields->as_array()) {
			field_schema field = parse_field(raw, std::format("{} field #{}", path, index++));
			if (!names.insert(field.name).second) {
				fail(path, std::format("duplicate field name '{}'", field.name));
			}
			if (!keys.insert(field.key).second) {
				fail(path, std::format("duplicate field key '{}'", field.key));
			}
			schema.fields.push_back(std::move(field));
		}

		compute_layout(schema, resolve);
		return schema;
	}

	enum_schema parse_enum_schema(const json::object& object)
	{
		check_keys(object, { "name", "items", "fallback" }, "enum");

		enum_schema schema;
		schema.name = read_string(object, "name", "enum", true);
		if (schema.name.empty()) {
			fail("enum", "'name' must not be empty");
		}
		const std::string path = std::format("enum '{}'", schema.name);

		const auto read_value = [&](const json::value& raw) {
			if (raw.is_int64()) {
				return raw.as_int64();
			}
			if (raw.is_uint64() && raw.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
				return static_cast<std::int64_t>(raw.as_uint64());
			}
			fail(path, "item values must be integers");
		};

		const json::value* items = find_value(object, "items");
		if (items && items->is_object()) {
			for (const auto& member : items->as_object()) {
				schema.items.push_back(enum_item{ std::string{ member.key() }, read_value(member.value()) });
			}
		} else if (items && items->is_array()) {
			for (const auto& raw : items->as_array()) {
				if (!raw.is_object()) {
					fail(path, "array items must be {\"name\": ..., \"value\": ...}");
				}
				const json::object& entry = raw.as_object();
				check_keys(entry, { "name", "value" }, path);
				const json::value* value = find_value(entry, "value");
				if (!value) {
					fail(path, "array item without 'value'");
				}
				schema.items.push_back(enum_item{ read_string(entry, "name", path, true), read_value(*value) });
			}
		} else {
			fail(path, "'items' must be an object or an array");
		}

		std::unordered_set<std::string> names;
		for (const auto& item : schema.items) {
			if (!names.insert(item.name).second) {
				fail(path, std::format("duplicate item '{}'", item.name));
			}
		}
		if (const json::value* fallback = find_value(object, "fallback")) {
			schema.fallback = read_value(*fallback);
		}
		return schema;
	}
}
