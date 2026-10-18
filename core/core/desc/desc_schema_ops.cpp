#include <snk/desc/desc_schema_ops.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_ops.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>

namespace desc
{
	namespace
	{
		constexpr std::string_view log_category = "desc";
		constexpr double float_tolerance = 1e-5;
		constexpr double double_tolerance = 1e-12;

		bool is_number(const json::value& value)
		{
			return value.is_int64() || value.is_uint64() || value.is_double();
		}

		bool is_integer(const json::value& value)
		{
			return value.is_int64() || value.is_uint64();
		}

		bool read_double(const json::value& value, double& out)
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

		bool read_int64(const json::value& value, std::int64_t& out)
		{
			if (value.is_int64()) {
				out = value.as_int64();
				return true;
			}
			if (value.is_uint64()) {
				if (value.as_uint64() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
					return false;
				}
				out = static_cast<std::int64_t>(value.as_uint64());
				return true;
			}
			if (value.is_double()) {
				const double number = value.as_double();
				if (!std::isfinite(number) || number != std::floor(number) || std::fabs(number) >= 9.2e18) {
					return false;
				}
				out = static_cast<std::int64_t>(number);
				return true;
			}
			return false;
		}

		bool read_uint64(const json::value& value, std::uint64_t& out)
		{
			if (value.is_uint64()) {
				out = value.as_uint64();
				return true;
			}
			std::int64_t number = 0;
			if (!read_int64(value, number) || number < 0) {
				return false;
			}
			out = static_cast<std::uint64_t>(number);
			return true;
		}

		bool read_integer_of_kind(field_kind kind, const json::value& value, std::int64_t& signed_out, std::uint64_t& unsigned_out)
		{
			switch (kind) {
			case field_kind::i32:
				return read_int64(value, signed_out)
					&& signed_out >= std::numeric_limits<std::int32_t>::min() && signed_out <= std::numeric_limits<std::int32_t>::max();
			case field_kind::i64:
				return read_int64(value, signed_out);
			case field_kind::u32:
			case field_kind::entity:
				return read_uint64(value, unsigned_out) && unsigned_out <= std::numeric_limits<std::uint32_t>::max();
			case field_kind::u64:
				return read_uint64(value, unsigned_out);
			default:
				return false;
			}
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

		bool read_components(field_kind kind, const json::value& value, float* out)
		{
			const std::size_t count = component_count(kind);
			if (!value.is_array() || value.as_array().size() != count) {
				return false;
			}
			for (std::size_t i = 0; i < count; ++i) {
				double number = 0.0;
				if (!read_double(value.as_array()[i], number)) {
					return false;
				}
				out[i] = static_cast<float>(number);
			}
			return true;
		}

		bool read_node_ref(const json::value& value, std::uint64_t& out)
		{
			if (!value.is_string()) {
				return false;
			}
			const std::string_view text{ value.as_string() };
			if (text.empty() || text.size() > 16) {
				return false;
			}
			const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 16);
			return ec == std::errc{} && end == text.data() + text.size();
		}

		bool is_handle_kind(field_kind kind)
		{
			switch (kind) {
			case field_kind::string:
			case field_kind::res_tag:
			case field_kind::desc_ref:
			case field_kind::array:
			case field_kind::custom: return true;
			default: return false;
			}
		}

		double tolerance_for(field_kind kind, field_kind element)
		{
			const field_kind effective = kind == field_kind::array ? element : kind;
			switch (effective) {
			case field_kind::f64: return double_tolerance;
			case field_kind::boolean:
			case field_kind::i32:
			case field_kind::u32:
			case field_kind::i64:
			case field_kind::u64:
			case field_kind::string:
			case field_kind::enumeration:
			case field_kind::res_tag:
			case field_kind::node_ref:
			case field_kind::entity: return 0.0;
			default: return float_tolerance;
			}
		}

		std::string escape_token(std::string_view token)
		{
			std::string result;
			for (char c : token) {
				if (c == '~') {
					result += "~0";
				} else if (c == '/') {
					result += "~1";
				} else {
					result.push_back(c);
				}
			}
			return result;
		}

		const json::value* pointer_get(const json::value& root, const std::vector<std::string>& tokens, std::size_t first)
		{
			const json::value* node = &root;
			for (std::size_t i = first; i < tokens.size(); ++i) {
				if (node->is_object()) {
					node = node->as_object().if_contains(tokens[i]);
				} else if (node->is_array()) {
					std::size_t index = 0;
					if (!json_array_index(tokens[i], node->as_array().size(), index) || index >= node->as_array().size()) {
						return nullptr;
					}
					node = &node->as_array()[index];
				} else {
					return nullptr;
				}
				if (!node) {
					return nullptr;
				}
			}
			return node;
		}

		const json::value* pointer_get_in_object(const json::object& root, std::string_view pointer)
		{
			std::vector<std::string> tokens;
			if (!json_pointer_tokens(pointer, tokens) || tokens.empty()) {
				return nullptr;
			}
			const json::value* head = root.if_contains(tokens.front());
			return head ? pointer_get(*head, tokens, 1) : nullptr;
		}

		// ---- runtime (blob) ops ----

		std::byte* blob_address(const void* obj, const field_ops& ops)
		{
			const auto* ref = static_cast<const dyn_ref*>(obj);
			return ref->blob->bytes.data() + ref->base + ops.offset;
		}

		template<typename T>
		T load(const std::byte* address)
		{
			T value;
			std::memcpy(&value, address, sizeof(T));
			return value;
		}

		template<typename T>
		void store(std::byte* address, T value)
		{
			std::memcpy(address, &value, sizeof(T));
		}

		json::value floats_to_json(const std::byte* address, std::size_t count)
		{
			json::array components;
			components.reserve(count);
			for (std::size_t i = 0; i < count; ++i) {
				components.push_back(json::value(static_cast<double>(load<float>(address + i * sizeof(float)))));
			}
			return json::value(std::move(components));
		}

		void rt_to_json(const field_ops& ops, const void* obj, json::value& out)
		{
			const std::byte* address = blob_address(obj, ops);
			switch (ops.kind) {
			case field_kind::boolean: out = json::value(load<std::uint8_t>(address) != 0); break;
			case field_kind::i32: out = json::value(static_cast<std::int64_t>(load<std::int32_t>(address))); break;
			case field_kind::i64: out = json::value(load<std::int64_t>(address)); break;
			case field_kind::u32:
			case field_kind::entity: out = json::value(static_cast<std::uint64_t>(load<std::uint32_t>(address))); break;
			case field_kind::u64: out = json::value(load<std::uint64_t>(address)); break;
			case field_kind::f32: out = json::value(static_cast<double>(load<float>(address))); break;
			case field_kind::f64: out = json::value(load<double>(address)); break;
			case field_kind::vec2:
			case field_kind::vec3:
			case field_kind::vec4:
			case field_kind::quat:
			case field_kind::color: out = floats_to_json(address, component_count(ops.kind)); break;
			case field_kind::enumeration: {
				const std::int64_t value = load<std::int32_t>(address);
				out = ops.enumeration ? enum_to_json(*ops.enumeration, value) : json::value(value);
				break;
			}
			case field_kind::node_ref: out = json::value(std::format("{:016x}", load<std::uint64_t>(address))); break;
			case field_kind::structure: {
				const auto* ref = static_cast<const dyn_ref*>(obj);
				dyn_ref child{ ref->blob, ref->base + ops.offset };
				json::object nested;
				serialize(*ops.nested, &child, nested);
				out = json::value(std::move(nested));
				break;
			}
			default: {
				const auto* ref = static_cast<const dyn_ref*>(obj);
				const std::uint32_t slot = load<std::uint32_t>(address);
				if (slot != 0 && slot <= ref->blob->handles.size()) {
					out = ref->blob->handles[slot - 1];
				} else if (ops.kind == field_kind::array) {
					out = json::value(json::array{});
				} else if (ops.kind == field_kind::custom) {
					out = json::value();
				} else {
					out = json::value(std::string_view{});
				}
				break;
			}
			}
		}

		bool rt_from_json(const field_ops& ops, void* obj, const json::value& in)
		{
			std::byte* address = blob_address(obj, ops);
			std::int64_t signed_value = 0;
			std::uint64_t unsigned_value = 0;
			switch (ops.kind) {
			case field_kind::boolean:
				if (!in.is_bool()) {
					return false;
				}
				store<std::uint8_t>(address, in.as_bool() ? 1 : 0);
				return true;
			case field_kind::i32:
				if (!read_integer_of_kind(ops.kind, in, signed_value, unsigned_value)) {
					return false;
				}
				store<std::int32_t>(address, static_cast<std::int32_t>(signed_value));
				return true;
			case field_kind::i64:
				if (!read_integer_of_kind(ops.kind, in, signed_value, unsigned_value)) {
					return false;
				}
				store<std::int64_t>(address, signed_value);
				return true;
			case field_kind::u32:
			case field_kind::entity:
				if (!read_integer_of_kind(ops.kind, in, signed_value, unsigned_value)) {
					return false;
				}
				store<std::uint32_t>(address, static_cast<std::uint32_t>(unsigned_value));
				return true;
			case field_kind::u64:
				if (!read_integer_of_kind(ops.kind, in, signed_value, unsigned_value)) {
					return false;
				}
				store<std::uint64_t>(address, unsigned_value);
				return true;
			case field_kind::f32:
			case field_kind::f64: {
				double number = 0.0;
				if (!read_double(in, number)) {
					return false;
				}
				if (ops.kind == field_kind::f32) {
					store<float>(address, static_cast<float>(number));
				} else {
					store<double>(address, number);
				}
				return true;
			}
			case field_kind::vec2:
			case field_kind::vec3:
			case field_kind::vec4:
			case field_kind::quat:
			case field_kind::color: {
				float components[4] = {};
				if (!read_components(ops.kind, in, components)) {
					return false;
				}
				std::memcpy(address, components, component_count(ops.kind) * sizeof(float));
				return true;
			}
			case field_kind::enumeration: {
				std::int64_t value = 0;
				if (ops.enumeration) {
					const auto converted = enum_from_json(*ops.enumeration, in);
					if (!converted) {
						return false;
					}
					value = *converted;
				} else if (!read_int64(in, value)) {
					return false;
				}
				if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
					return false;
				}
				store<std::int32_t>(address, static_cast<std::int32_t>(value));
				return true;
			}
			case field_kind::node_ref:
				if (!read_node_ref(in, unsigned_value)) {
					return false;
				}
				store<std::uint64_t>(address, unsigned_value);
				return true;
			case field_kind::structure: {
				if (!in.is_object()) {
					return false;
				}
				const auto* ref = static_cast<const dyn_ref*>(obj);
				dyn_ref child{ ref->blob, ref->base + ops.offset };
				deserialize(*ops.nested, &child, in.as_object());
				return true;
			}
			default: {
				if (!json_fits(ops.kind, ops.element, ops.enumeration.get(), in)) {
					return false;
				}
				auto& handles = static_cast<const dyn_ref*>(obj)->blob->handles;
				const std::uint32_t slot = load<std::uint32_t>(address);
				if (slot != 0 && slot <= handles.size()) {
					handles[slot - 1] = in;
				} else {
					handles.push_back(in);
					store<std::uint32_t>(address, static_cast<std::uint32_t>(handles.size()));
				}
				return true;
			}
			}
		}

		bool rt_equal(const field_ops& ops, const void* a, const void* b)
		{
			json::value left;
			json::value right;
			rt_to_json(ops, a, left);
			rt_to_json(ops, b, right);
			return json_equal(left, right);
		}

		void rt_assign(const field_ops& ops, void* dst, const void* src)
		{
			if (ops.kind == field_kind::structure) {
				const auto* from = static_cast<const dyn_ref*>(src);
				const auto* to = static_cast<const dyn_ref*>(dst);
				dyn_ref from_child{ from->blob, from->base + ops.offset };
				dyn_ref to_child{ to->blob, to->base + ops.offset };
				for (const auto& nested : ops.nested->ops) {
					nested.assign(nested, &to_child, &from_child);
				}
				return;
			}
			if (is_handle_kind(ops.kind)) {
				json::value value;
				rt_to_json(ops, src, value);
				if (!value.is_null()) {
					rt_from_json(ops, dst, value);
				}
				return;
			}
			std::memmove(blob_address(dst, ops), blob_address(src, ops), layout_of(ops.kind).size);
		}

		std::shared_ptr<const enum_schema> resolve_enum(const field_schema& field, const schema_registry& resolve)
		{
			const bool wants_enum = field.kind == field_kind::enumeration || (field.kind == field_kind::array && field.element == field_kind::enumeration);
			if (!wants_enum || field.ref_type.empty()) {
				return nullptr;
			}
			const enum_schema* found = resolve.find_enum(field.ref_type);
			return found ? std::make_shared<const enum_schema>(*found) : nullptr;
		}

		void apply_defaults(const schema_binding& binding, dyn_ref ref)
		{
			for (std::size_t i = 0; i < binding.ops.size(); ++i) {
				const field_ops& ops = binding.ops[i];
				const json::value& value = binding.schema->fields[i].default_value;
				if (!value.is_null()) {
					ops.from_json(ops, &ref, value);
				} else if (ops.nested) {
					apply_defaults(*ops.nested, dyn_ref{ ref.blob, ref.base + ops.offset });
				}
			}
		}

		// ---- diff / validate ----

		void diff_untyped(const std::string& prefix, const json::value& base, const json::value& edited, std::vector<json_change>& out)
		{
			for (const auto& member : edited.as_object()) {
				const std::string pointer = prefix + "/" + escape_token(member.key());
				const json::value* before = base.as_object().if_contains(member.key());
				if (!before) {
					out.push_back({ pointer, member.value() });
				} else if (before->is_object() && member.value().is_object()) {
					diff_untyped(pointer, *before, member.value(), out);
				} else if (!json_equal(*before, member.value(), float_tolerance)) {
					out.push_back({ pointer, member.value() });
				}
			}
		}

		void diff_fields(const std::string& prefix, const type_schema& schema, const json::value& base, const json::value& edited,
			const schema_registry* resolve, std::vector<json_change>& out)
		{
			for (const auto& field : schema.fields) {
				const json::value* after = json_pointer_get(edited, field.key);
				if (!after) {
					continue;
				}
				const json::value* before = json_pointer_get(base, field.key);
				const std::string pointer = prefix + field.key;
				if (field.kind == field_kind::structure && before && before->is_object() && after->is_object()) {
					const type_schema* nested = resolve ? resolve->find(field.ref_type) : nullptr;
					if (nested) {
						diff_fields(pointer, *nested, *before, *after, resolve, out);
					} else {
						diff_untyped(pointer, *before, *after, out);
					}
					continue;
				}
				if (!before || !json_equal(*before, *after, tolerance_for(field.kind, field.element))) {
					out.push_back({ pointer, *after });
				}
			}
		}

		const enum_schema* enum_of(const field_schema& field, const schema_registry* resolve)
		{
			return resolve && !field.ref_type.empty() ? resolve->find_enum(field.ref_type) : nullptr;
		}

		override_status check_override(const type_schema& schema, std::string_view pointer, const json::value& value, const schema_registry* resolve)
		{
			for (const auto& field : schema.fields) {
				const bool is_alias = std::ranges::find(field.read_aliases, pointer, [](const std::string& alias) { return std::string_view{ alias }; }) != field.read_aliases.end();
				if (pointer == field.key || is_alias) {
					return json_fits(field.kind, field.element, enum_of(field, resolve), value) ? override_status::valid : override_status::bad_value;
				}
			}
			for (const auto& field : schema.fields) {
				if (field.key.empty() || pointer.size() <= field.key.size() || pointer.substr(0, field.key.size()) != field.key || pointer[field.key.size()] != '/') {
					continue;
				}
				const std::string_view rest = pointer.substr(field.key.size());
				switch (field.kind) {
				case field_kind::structure: {
					const type_schema* nested = resolve ? resolve->find(field.ref_type) : nullptr;
					return nested ? check_override(*nested, rest, value, resolve) : override_status::valid;
				}
				case field_kind::array: {
					std::vector<std::string> tokens;
					if (!json_pointer_tokens(rest, tokens) || tokens.empty()) {
						return override_status::stale;
					}
					std::size_t index = 0;
					if (!json_array_index(tokens.front(), std::numeric_limits<std::size_t>::max() - 1, index)) {
						return override_status::stale;
					}
					if (tokens.size() > 1) {
						return override_status::valid;
					}
					return json_fits(field.element, field_kind::boolean, enum_of(field, resolve), value) ? override_status::valid : override_status::bad_value;
				}
				case field_kind::custom:
					return override_status::valid;
				case field_kind::vec2:
				case field_kind::vec3:
				case field_kind::vec4:
				case field_kind::quat:
				case field_kind::color: {
					std::vector<std::string> tokens;
					std::size_t index = 0;
					if (!json_pointer_tokens(rest, tokens) || tokens.size() != 1 || !json_array_index(tokens.front(), component_count(field.kind) - 1, index) || tokens.front() == "-") {
						return override_status::stale;
					}
					return is_number(value) ? override_status::valid : override_status::bad_value;
				}
				default:
					break;
				}
			}
			return override_status::stale;
		}
	}

	bool json_equal(const json::value& a, const json::value& b, double tolerance)
	{
		if (is_number(a) && is_number(b)) {
			if (is_integer(a) && is_integer(b)) {
				if (a.is_int64() && b.is_int64()) {
					return a.as_int64() == b.as_int64();
				}
				if (a.is_uint64() && b.is_uint64()) {
					return a.as_uint64() == b.as_uint64();
				}
				const json::value& signed_value = a.is_int64() ? a : b;
				const json::value& unsigned_value = a.is_int64() ? b : a;
				return signed_value.as_int64() >= 0 && static_cast<std::uint64_t>(signed_value.as_int64()) == unsigned_value.as_uint64();
			}
			double left = 0.0;
			double right = 0.0;
			read_double(a, left);
			read_double(b, right);
			if (left == right) {
				return true;
			}
			return std::fabs(left - right) <= tolerance * std::max({ 1.0, std::fabs(left), std::fabs(right) });
		}
		if (a.is_array() && b.is_array()) {
			const auto& left = a.as_array();
			const auto& right = b.as_array();
			if (left.size() != right.size()) {
				return false;
			}
			for (std::size_t i = 0; i < left.size(); ++i) {
				if (!json_equal(left[i], right[i], tolerance)) {
					return false;
				}
			}
			return true;
		}
		if (a.is_object() && b.is_object()) {
			const auto& left = a.as_object();
			const auto& right = b.as_object();
			if (left.size() != right.size()) {
				return false;
			}
			for (const auto& member : left) {
				const json::value* other = right.if_contains(member.key());
				if (!other || !json_equal(member.value(), *other, tolerance)) {
					return false;
				}
			}
			return true;
		}
		return a == b;
	}

	bool json_fits(field_kind kind, field_kind element, const enum_schema* enumeration, const json::value& value)
	{
		std::int64_t signed_value = 0;
		std::uint64_t unsigned_value = 0;
		switch (kind) {
		case field_kind::boolean: return value.is_bool();
		case field_kind::i32:
		case field_kind::i64:
		case field_kind::u32:
		case field_kind::u64:
		case field_kind::entity: return read_integer_of_kind(kind, value, signed_value, unsigned_value);
		case field_kind::f32:
		case field_kind::f64: return is_number(value);
		case field_kind::vec2:
		case field_kind::vec3:
		case field_kind::vec4:
		case field_kind::quat:
		case field_kind::color: {
			float components[4] = {};
			return read_components(kind, value, components);
		}
		case field_kind::string:
		case field_kind::res_tag: return value.is_string();
		case field_kind::enumeration:
			if (enumeration) {
				return enum_from_json(*enumeration, value).has_value();
			}
			return value.is_string() || is_integer(value);
		case field_kind::desc_ref: return value.is_string() || value.is_object();
		case field_kind::node_ref: return read_node_ref(value, unsigned_value);
		case field_kind::structure: return value.is_object();
		case field_kind::array:
			if (!value.is_array()) {
				return false;
			}
			return std::ranges::all_of(value.as_array(), [&](const json::value& item) {
				return json_fits(element, field_kind::boolean, enumeration, item);
			});
		case field_kind::custom: return true;
		}
		return false;
	}

	std::optional<std::int64_t> enum_from_json(const enum_schema& schema, const json::value& value)
	{
		if (value.is_string()) {
			if (const enum_item* item = schema.find(std::string_view{ value.as_string() })) {
				return item->value;
			}
			return schema.fallback;
		}
		std::int64_t number = 0;
		if (is_integer(value) && read_int64(value, number) && schema.find_value(number)) {
			return number;
		}
		return std::nullopt;
	}

	json::value enum_to_json(const enum_schema& schema, std::int64_t value)
	{
		if (const enum_item* item = schema.find_value(value)) {
			return json::value(item->name);
		}
		return json::value(value);
	}

	bool json_to_double(const json::value& value, double& out)
	{
		return read_double(value, out);
	}

	bool json_to_int64(const json::value& value, std::int64_t& out)
	{
		return read_int64(value, out);
	}

	bool json_to_uint64(const json::value& value, std::uint64_t& out)
	{
		return read_uint64(value, out);
	}

	bool is_valid_value(const json::value& value)
	{
		if (value.is_null()) {
			return false;
		}
		if (value.is_string()) {
			return !value.as_string().empty();
		}
		if (value.is_array()) {
			return !value.as_array().empty();
		}
		return true;
	}

	std::shared_ptr<const schema_binding> bind_runtime(type_schema schema, const schema_registry& resolve)
	{
		auto binding = std::make_shared<schema_binding>();
		binding->schema = std::make_shared<const type_schema>(std::move(schema));
		binding->object_type = &typeid(dyn_ref);

		const type_schema& owned = *binding->schema;
		binding->ops.reserve(owned.fields.size());
		for (const auto& field : owned.fields) {
			field_ops ops;
			ops.to_json = &rt_to_json;
			ops.from_json = &rt_from_json;
			ops.equal = &rt_equal;
			ops.assign = &rt_assign;
			ops.kind = field.kind;
			ops.element = field.element;
			ops.offset = field.offset;
			ops.enumeration = resolve_enum(field, resolve);

			std::uint32_t size = layout_of(field.kind).size;
			if (field.kind == field_kind::structure) {
				const type_schema* nested = resolve.find(field.ref_type);
				if (!nested) {
					throw std::runtime_error(std::format("schema '{}': structure field '{}' references unknown schema '{}'", owned.name, field.name, field.ref_type));
				}
				ops.nested = bind_runtime(*nested, resolve);
				size = nested->layout.size;
			}
			if (field.offset + size > owned.layout.size) {
				throw std::runtime_error(std::format("schema '{}': field '{}' does not fit the blob layout (compute_layout was not run?)", owned.name, field.name));
			}
			binding->ops.push_back(std::move(ops));
		}
		return binding;
	}

	dyn_blob make_blob(const schema_binding& binding)
	{
		dyn_blob blob;
		blob.bytes.assign(binding.schema->layout.size, std::byte{ 0 });
		dyn_ref ref{ &blob, 0 };
		apply_defaults(binding, ref);
		return blob;
	}

	void serialize(const schema_binding& binding, const void* obj, json::object& out)
	{
		json::value document(std::move(out));
		const auto& fields = binding.schema->fields;
		for (std::size_t i = 0; i < fields.size(); ++i) {
			const field_schema& field = fields[i];
			json::value value;
			binding.ops[i].to_json(binding.ops[i], obj, value);
			if (value.is_null()) {
				continue;
			}
			if (field.write == write_policy::if_valid && !is_valid_value(value)) {
				continue;
			}
			if (field.write == write_policy::if_not_default && json_equal(value, field.default_value)) {
				continue;
			}
			if (!json_pointer_set(document, field.key, std::move(value))) {
				egLOG_WARN(log_category, "'{}': cannot write field '{}' at '{}'", binding.schema->name, field.name, field.key);
			}
		}
		out = std::move(document.as_object());
	}

	void deserialize(const schema_binding& binding, void* obj, const json::object& in)
	{
		const auto& fields = binding.schema->fields;
		std::vector<std::string> known;
		known.reserve(fields.size());
		std::vector<std::string> tokens;
		const auto remember = [&](const std::string& pointer) {
			if (json_pointer_tokens(pointer, tokens) && !tokens.empty()) {
				known.push_back(tokens.front());
			}
		};

		for (std::size_t i = 0; i < fields.size(); ++i) {
			const field_schema& field = fields[i];
			remember(field.key);
			const json::value* value = pointer_get_in_object(in, field.key);
			for (const auto& alias : field.read_aliases) {
				remember(alias);
				if (!value) {
					value = pointer_get_in_object(in, alias);
				}
			}
			if (value && !binding.ops[i].from_json(binding.ops[i], obj, *value)) {
				egLOG_WARN(log_category, "'{}': value of '{}' does not fit {}, keeping the current one", binding.schema->name, field.key, to_string(field.kind));
			}
		}

		for (const auto& member : in) {
			const std::string_view key{ member.key() };
			if (key.starts_with("__") || std::ranges::find(known, key) != known.end()) {
				continue;
			}
			egLOG_DEBUG(log_category, "'{}': ignoring unknown key '{}'", binding.schema->name, key);
		}
	}

	void copy(const schema_binding& src_binding, const void* src, const schema_binding& dst_binding, void* dst)
	{
		if (src == dst && &src_binding == &dst_binding) {
			return;
		}
		if (src_binding.object_type && dst_binding.object_type && *src_binding.object_type == *dst_binding.object_type
			&& (src_binding.schema == dst_binding.schema || *src_binding.schema == *dst_binding.schema)) {
			for (const auto& ops : dst_binding.ops) {
				ops.assign(ops, dst, src);
			}
			return;
		}
		egLOG_WARN(log_category, "copying '{}' to '{}' goes through JSON: only the keys both know are copied", src_binding.schema->name, dst_binding.schema->name);
		json::object transfer;
		serialize(src_binding, src, transfer);
		deserialize(dst_binding, dst, transfer);
	}

	bool equal(const schema_binding& binding, const void* a, const void* b)
	{
		return std::ranges::all_of(binding.ops, [&](const field_ops& ops) { return ops.equal(ops, a, b); });
	}

	std::vector<json_change> diff(const type_schema& schema, const json::value& base, const json::value& edited, const schema_registry* resolve)
	{
		std::vector<json_change> changes;
		diff_fields({}, schema, base, edited, resolve, changes);
		return changes;
	}

	override_status validate_override(const type_schema& schema, const json_change& record, const schema_registry* resolve)
	{
		const override_status status = check_override(schema, record.pointer, record.value, resolve);
		if (status == override_status::stale) {
			egLOG_WARN(log_category, "stale override '{}' for '{}': no such field, the record is kept", record.pointer, schema.name);
		} else if (status == override_status::bad_value) {
			egLOG_WARN(log_category, "override '{}' for '{}' has a value that does not fit the field", record.pointer, schema.name);
		}
		return status;
	}
}
