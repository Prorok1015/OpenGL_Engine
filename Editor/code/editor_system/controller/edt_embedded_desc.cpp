#include "edt_embedded_desc.h"
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/resource/res_diagnostics.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <vector>

namespace
{
	namespace json = boost::json;

	enum class numbers { as_is, float32 };

	// What the numbers below a field are, by the kind of the field (or of its elements).
	numbers numbers_of(desc::field_kind kind)
	{
		switch (kind) {
		case desc::field_kind::f32:
		case desc::field_kind::vec2:
		case desc::field_kind::vec3:
		case desc::field_kind::vec4:
		case desc::field_kind::quat:
		case desc::field_kind::color:
			return numbers::float32;
		default:
			return numbers::as_is;
		}
	}

	bool is_tag_kind(desc::field_kind kind)
	{
		return kind == desc::field_kind::res_tag || kind == desc::field_kind::desc_ref;
	}

	// 0.1f widened to double is 0.10000000149011612: the shortest text that reads back as the same float is the value.
	double shortest_float(double value)
	{
		const float narrow = static_cast<float>(value);
		if (!std::isfinite(narrow)) return value;
		char text[64];
		const auto done = std::to_chars(text, text + sizeof(text) - 1, narrow);
		if (done.ec != std::errc{}) return value;
		*done.ptr = '\0';
		return std::strtod(text, nullptr);
	}

	// 2^53: below it a double holds every integer, so an integral double is the integer it says.
	constexpr double k_exact_integer_limit = 9007199254740992.0;

	json::value number(const json::value& value, numbers kind)
	{
		if (value.is_int64() || value.is_uint64()) return value;
		double x = value.get_double();
		if (kind == numbers::float32) x = shortest_float(x);
		if (std::isfinite(x) && x == std::floor(x) && std::fabs(x) <= k_exact_integer_limit) return json::value(static_cast<std::int64_t>(x));
		return json::value(x);
	}

	// The value at `pointer` taken out of `root`, and the objects that were left empty on the way.
	void remove_with_empty_parents(json::value& root, const std::string& pointer)
	{
		desc::json_pointer_remove(root, pointer);
		for (std::string parent = pointer.substr(0, pointer.rfind('/')); !parent.empty(); parent = parent.substr(0, parent.rfind('/'))) {
			const json::value* left = desc::json_pointer_get(root, parent);
			if (!left || !left->is_object() || !left->as_object().empty()) break;
			desc::json_pointer_remove(root, parent);
		}
	}

	// A copy with the keys of every object below in alphabetical order and the numbers made canonical: what no field of the schema names has one form too.
	json::value sorted(const json::value& value)
	{
		if (value.is_number()) return number(value, numbers::as_is);
		if (value.is_object()) {
			std::vector<std::string> keys;
			for (const auto& member : value.as_object()) keys.emplace_back(member.key());
			std::ranges::sort(keys);
			json::object out;
			for (const std::string& key : keys) out[key] = sorted(value.as_object().at(key));
			return out;
		}
		if (value.is_array()) {
			json::array out;
			for (const json::value& item : value.as_array()) out.push_back(sorted(item));
			return out;
		}
		return value;
	}

	// What the value at a field is read as: the schema of the object it holds, the numbers of its kind, and whether it is a reference.
	struct context
	{
		const desc::type_schema* schema = nullptr;
		numbers kind = numbers::as_is;
		bool reference = false;
	};

	struct canonicalizer
	{
		const desc::schema_registry& schemas;
		const edt::reference_normalizer& normalize;

		context of_field(const desc::field_schema* field) const
		{
			context out;
			if (!field) return out;
			const desc::field_kind element = field->kind == desc::field_kind::array || field->kind == desc::field_kind::map ? field->element : field->kind;
			out.kind = numbers_of(element);
			out.reference = is_tag_kind(element);
			const bool nested = field->kind == desc::field_kind::structure || element == desc::field_kind::structure;
			if (nested && !field->ref_type.empty()) out.schema = schemas.find(field->ref_type);
			return out;
		}

		json::value value(const json::value& in, const context& at) const
		{
			if (in.is_number()) return number(in, at.kind);
			if (in.is_array()) {
				json::array out;
				for (const json::value& item : in.as_array()) out.push_back(value(item, at));
				return out;
			}
			if (in.is_string() && at.reference && normalize) return normalize(in);
			if (!in.is_object()) return in;
			if (at.reference && normalize && !in.as_object().contains("__type")) return normalize(in);

			const json::object& object = in.as_object();
			context inside = at;
			if (const json::value* type = object.if_contains("__type"); type && type->is_string()) {
				inside = context{};
				inside.schema = schemas.find(std::string_view(type->as_string()));
			}
			// The key of a field is a JSON pointer (`/shininess`, `/uniforms/albedo`): the fields come in the order of the schema, each placed by its
			// pointer, and what no field names follows, alphabetically.
			const json::value source(object);
			json::value left(object);
			json::value out = json::object{};
			// The header of an inline desc comes first, as the files write it.
			for (const char* header : { "__type", "__version", "__parent", "__guid" }) {
				if (const json::value* held = object.if_contains(header)) {
					out.as_object()[header] = *held;
					left.as_object().erase(header);
				}
			}
			if (inside.schema) {
				for (const desc::field_schema& field : inside.schema->fields) {
					const json::value* held = desc::json_pointer_get(source, field.key);
					if (!held) continue;
					desc::json_pointer_set(out, field.key, value(*held, of_field(&field)));
					remove_with_empty_parents(left, field.key);
				}
			}
			json::object& result = out.as_object();
			if (left.is_object()) desc::json_deep_merge(result, sorted(left).as_object());
			return out;
		}
	};

	bool is_inline_desc(const json::value& value)
	{
		const json::object* object = value.if_object();
		const json::value* type = object ? object->if_contains("__type") : nullptr;
		return type && type->is_string();
	}

	// What `own` (already canonical) sets over `base` (canonical too), keeping the order of `own`. `prefix`: the pointer of `own` below the object the
	// fields of `schema` are keyed from (a field's key is a pointer such as `/uniforms/albedo`).
	json::object diff(const desc::schema_registry& schemas, const desc::type_schema* schema, const json::object& own, const json::object& base,
		const std::string& prefix, int depth)
	{
		json::object out;
		for (const auto& member : own) {
			const std::string key(member.key());
			const json::value* held = nullptr;
			if (const auto was = base.find(key); was != base.end()) held = &was->value();
			if (held && *held == member.value()) continue;
			const std::string pointer = prefix + "/" + res::json_pointer_token(key);
			const desc::field_schema* field = schema ? schema->find_field(pointer) : nullptr;
			const bool whole = !member.value().is_object() || !held || !held->is_object() || is_inline_desc(member.value()) || is_inline_desc(*held)
				|| (field && (field->kind == desc::field_kind::map || is_tag_kind(field->kind) || field->kind == desc::field_kind::custom)) || depth >= 8;
			if (whole) {
				out[key] = member.value();
				continue;
			}
			// Into a structure the keys of its own schema start again from its root; into a plain container the same schema goes on one pointer deeper.
			const desc::type_schema* inner = field && field->kind == desc::field_kind::structure ? schemas.find(field->ref_type) : schema;
			const std::string inner_prefix = field && field->kind == desc::field_kind::structure ? std::string{} : pointer;
			json::object nested = diff(schemas, inner, member.value().as_object(), held->as_object(), inner_prefix, depth + 1);
			if (!nested.empty()) out[key] = std::move(nested);
		}
		return out;
	}
}

boost::json::object edt::canonical_embedded_value(const desc::schema_registry& schemas, const desc::type_schema& schema, const boost::json::object& own,
	const boost::json::object& base, int version, std::string_view parent, const reference_normalizer& normalize)
{
	json::object fields = own;
	json::object below = base;
	for (const char* header : { "__type", "__version", "__parent", "__guid" }) {
		fields.erase(header);
		below.erase(header);
	}
	const canonicalizer canon{ schemas, normalize };
	context root;
	root.schema = &schema;
	const json::object own_form = canon.value(fields, root).as_object();
	const json::object base_form = canon.value(below, root).as_object();

	json::object out;
	out["__type"] = schema.name;
	out["__version"] = version;
	if (!parent.empty()) out["__parent"] = std::string(parent);
	for (auto& member : diff(schemas, &schema, own_form, base_form, std::string{}, 0)) out[member.key()] = std::move(member.value());
	return out;
}
