#include "edt_embedded_desc.h"
#include <snk/desc/desc_schema.h>
#include <algorithm>
#include <set>
#include <vector>

namespace
{
	bool same_value(const boost::json::value& a, const boost::json::value& b)
	{
		if (a.is_number() && b.is_number()) return a.to_number<double>() == b.to_number<double>();
		if (a.is_object() && b.is_object()) {
			const auto& left = a.as_object();
			const auto& right = b.as_object();
			if (left.size() != right.size()) return false;
			return std::ranges::all_of(left, [&](const auto& member) {
				const auto found = right.find(member.key());
				return found != right.end() && same_value(member.value(), found->value());
			});
		}
		if (a.is_array() && b.is_array()) {
			const auto& left = a.as_array();
			const auto& right = b.as_array();
			if (left.size() != right.size()) return false;
			for (std::size_t i = 0; i < left.size(); ++i) {
				if (!same_value(left[i], right[i])) return false;
			}
			return true;
		}
		return a == b;
	}

	// A copy with the keys of every object below in alphabetical order: what the schema does not describe still has one form.
	boost::json::value sorted(const boost::json::value& value)
	{
		if (value.is_object()) {
			std::vector<std::string> keys;
			for (const auto& member : value.as_object()) keys.emplace_back(member.key());
			std::ranges::sort(keys);
			boost::json::object out;
			for (const std::string& key : keys) out[key] = sorted(value.as_object().at(key));
			return out;
		}
		if (value.is_array()) {
			boost::json::array out;
			for (const auto& item : value.as_array()) out.push_back(sorted(item));
			return out;
		}
		return value;
	}

	// What `own` sets over `base`, in the order of `schema` (null: no schema, alphabetical); empty when it sets nothing new.
	boost::json::object canonical_fields(const desc::schema_registry& schemas, const desc::type_schema* schema, const boost::json::object& own,
		const boost::json::object* base, int depth)
	{
		boost::json::object out;
		std::set<std::string> done;
		const auto take = [&](const std::string& key, const desc::field_schema* field) {
			const auto found = own.find(key);
			if (found == own.end() || !done.insert(key).second) return;
			const boost::json::value* held = nullptr;
			if (base) {
				if (const auto was = base->find(key); was != base->end()) held = &was->value();
			}
			if (held && same_value(*held, found->value())) return;
			if (found->value().is_object() && held && held->is_object() && depth < 8) {
				const desc::type_schema* inner = field && field->kind == desc::field_kind::structure ? schemas.find(field->ref_type) : nullptr;
				boost::json::object nested = canonical_fields(schemas, inner, found->value().as_object(), &held->as_object(), depth + 1);
				if (!nested.empty()) out[key] = std::move(nested);
				return;
			}
			out[key] = sorted(found->value());
		};
		if (schema) {
			for (const desc::field_schema& field : schema->fields) take(field.key, &field);
		}
		std::vector<std::string> rest;
		for (const auto& member : own) {
			const std::string key(member.key());
			if (!done.contains(key)) rest.push_back(key);
		}
		std::ranges::sort(rest);
		for (const std::string& key : rest) take(key, nullptr);
		return out;
	}
}

boost::json::object edt::canonical_embedded_value(const desc::schema_registry& schemas, const desc::type_schema& schema, const boost::json::object& own,
	const boost::json::object& base, int version, std::string_view parent)
{
	boost::json::object out;
	out["__type"] = schema.name;
	out["__version"] = version;
	if (!parent.empty()) out["__parent"] = std::string(parent);
	boost::json::object fields = own;
	for (const char* header : { "__type", "__version", "__parent", "__guid" }) fields.erase(header);
	for (auto& member : canonical_fields(schemas, &schema, fields, &base, 0)) out[member.key()] = std::move(member.value());
	return out;
}
