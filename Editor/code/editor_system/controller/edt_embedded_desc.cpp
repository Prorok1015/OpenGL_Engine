#include "edt_embedded_desc.h"
#include <snk/desc/desc_schema.h>
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
			json::object out;
			std::set<std::string> done;
			const auto take = [&](const std::string& key, const desc::field_schema* field) {
				const auto found = object.find(key);
				if (found == object.end() || !done.insert(key).second) return;
				out[key] = value(found->value(), of_field(field));
			};
			if (inside.schema) {
				for (const desc::field_schema& field : inside.schema->fields) take(field.key, &field);
			}
			std::vector<std::string> rest;
			for (const auto& member : object) {
				const std::string key(member.key());
				if (!done.contains(key)) rest.push_back(key);
			}
			std::ranges::sort(rest);
			for (const std::string& key : rest) take(key, nullptr);
			return out;
		}
	};

	bool is_inline_desc(const json::value& value)
	{
		const json::object* object = value.if_object();
		const json::value* type = object ? object->if_contains("__type") : nullptr;
		return type && type->is_string();
	}

	// What `own` (already canonical) sets over `base` (canonical too), keeping the order of `own`.
	json::object diff(const desc::schema_registry& schemas, const desc::type_schema* schema, const json::object& own, const json::object& base, int depth)
	{
		json::object out;
		for (const auto& member : own) {
			const std::string key(member.key());
			const json::value* held = nullptr;
			if (const auto was = base.find(key); was != base.end()) held = &was->value();
			if (held && *held == member.value()) continue;
			const desc::field_schema* field = schema ? schema->find_field(key) : nullptr;
			const bool whole = !member.value().is_object() || !held || !held->is_object() || is_inline_desc(member.value()) || is_inline_desc(*held)
				|| (field && (field->kind == desc::field_kind::map || is_tag_kind(field->kind))) || depth >= 8;
			if (whole) {
				out[key] = member.value();
				continue;
			}
			const desc::type_schema* inner = field && field->kind == desc::field_kind::structure ? schemas.find(field->ref_type) : nullptr;
			json::object nested = diff(schemas, inner, member.value().as_object(), held->as_object(), depth + 1);
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
	for (auto& member : diff(schemas, &schema, own_form, base_form, 0)) out[member.key()] = std::move(member.value());
	return out;
}
