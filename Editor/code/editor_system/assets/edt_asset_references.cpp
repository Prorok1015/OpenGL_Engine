#include "edt_asset_references.h"
#include "edt_reference.h"
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;

namespace
{
	// Typed objects inside typed objects: deeper than any file the editor writes.
	constexpr int k_max_depth = 128;

	std::string_view view_of(const json::string& text)
	{
		return std::string_view(text.data(), text.size());
	}

	std::string token_of(const json::string& key)
	{
		return res::json_pointer_token(view_of(key));
	}

	bool is_reference_shaped(const json::object& object)
	{
		return !object.empty() && std::ranges::all_of(object, [](const auto& member) {
			const std::string_view key = view_of(member.key());
			return key == "guid" || key == "path";
		});
	}

	// The schemes a file of the project can be named by: res://, and engine:// for #891.
	bool names_project_file(std::string_view value)
	{
		return value.starts_with("res://") || value.starts_with("engine://");
	}

	// A node of a prefab, or a prefab's root: its "components" map names each component by its type.
	bool is_prefab_node(const json::object& object, std::string_view type)
	{
		return type == "prefab_desc" || object.contains("__id") || object.contains("transform") || object.contains("children");
	}

	struct collector
	{
		std::vector<edt::raw_reference>& out;

		void add(std::string pointer, std::string path, std::optional<res::asset_id> guid, const std::string& owner_type,
			const std::string& owner_pointer, edt::reference_place place)
		{
			out.push_back(edt::raw_reference{ std::move(pointer), std::move(path), guid, owner_type, owner_pointer, place });
		}

		void text(std::string_view value, const std::string& pointer, const std::string& owner_type, const std::string& owner_pointer,
			edt::reference_place place)
		{
			if (!names_project_file(value)) return;
			add(pointer, std::string(value), std::nullopt, owner_type, owner_pointer, place);
		}

		void value(const json::value& item, const std::string& pointer, const std::string& owner_type, const std::string& owner_pointer, int depth)
		{
			if (depth > k_max_depth) return;
			if (const json::string* string = item.if_string()) {
				text(view_of(*string), pointer, owner_type, owner_pointer, edt::reference_place::value);
			} else if (const json::object* object = item.if_object()) {
				members(*object, pointer, owner_type, owner_pointer, depth);
			} else if (const json::array* items = item.if_array()) {
				// Numeric payloads (geometry) can be megabytes long and never hold a reference.
				if (!items->empty() && items->front().is_number()) return;
				for (std::size_t i = 0; i < items->size(); ++i) {
					value((*items)[i], pointer + "/" + std::to_string(i), owner_type, owner_pointer, depth + 1);
				}
			}
		}

		void members(const json::object& object, const std::string& pointer, const std::string& outer_type, const std::string& outer_pointer, int depth)
		{
			const json::value* type = object.if_contains("__type");
			if (!type && is_reference_shaped(object)) {
				// Only a small object is copied: one that has no key but "guid" and "path".
				if (const auto read = res::reference_from_json(json::value(object));
					read && (!read->guid.is_nil() || names_project_file(read->path.view()))) {
					add(pointer, std::string(read->path.view()), read->guid.is_nil() ? std::nullopt : std::optional{ read->guid }, outer_type,
						outer_pointer, edt::reference_place::value);
				}
				return;
			}
			const bool typed = type && type->is_string();
			const std::string owner_type = typed ? std::string(view_of(type->as_string())) : outer_type;
			const std::string& owner_pointer = typed ? pointer : outer_pointer;
			const bool prefab_node = is_prefab_node(object, typed ? std::string_view(owner_type) : std::string_view{});
			for (const auto& member : object) {
				const std::string_view key = view_of(member.key());
				const std::string at = pointer + "/" + token_of(member.key());
				if (key == "__parent") {
					if (const json::string* parent = member.value().if_string()) {
						text(view_of(*parent), at, owner_type, owner_pointer, edt::reference_place::parent);
					}
					continue;
				}
				if (key.starts_with("__")) continue;
				// The components of a prefab node: one given as a tag is the desc it is made of; one without "__type" is of
				// the type its key names, as the prefab loader reads it.
				if (prefab_node && key == "components" && member.value().is_object()) {
					for (const auto& component : member.value().as_object()) {
						const std::string component_at = at + "/" + token_of(component.key());
						if (const json::string* tag = component.value().if_string()) {
							text(view_of(*tag), component_at, owner_type, owner_pointer, edt::reference_place::component);
						} else if (component.value().is_object()) {
							members(component.value().as_object(), component_at, std::string(view_of(component.key())), component_at, depth + 1);
						} else {
							value(component.value(), component_at, owner_type, owner_pointer, depth + 1);
						}
					}
					continue;
				}
				value(member.value(), at, owner_type, owner_pointer, depth + 1);
			}
		}
	};

	bool is_tag_kind(desc::field_kind kind)
	{
		return kind == desc::field_kind::res_tag || kind == desc::field_kind::desc_ref;
	}

	std::string pointer_of(std::string_view key)
	{
		return key.starts_with('/') ? std::string(key) : "/" + std::string(key);
	}

	// Whether `tokens` (a pointer inside an object of `schema`) name a res_tag or desc_ref value: the field itself, an
	// element of a list or map of them, or such a field in a structure, by every key a field is read under.
	bool reference_field_at(const desc::desc_system& descs, const desc::type_schema& schema, std::span<const std::string> tokens, int depth)
	{
		if (tokens.empty() || depth > 32) return false;
		std::vector<std::string> key_tokens;
		for (const desc::field_schema& field : schema.fields) {
			std::vector<std::string_view> keys{ field.key };
			for (const std::string& alias : field.read_aliases) keys.push_back(alias);
			for (const std::string_view key : keys) {
				if (!desc::json_pointer_tokens(pointer_of(key), key_tokens) || key_tokens.empty() || key_tokens.size() > tokens.size()
					|| !std::equal(key_tokens.begin(), key_tokens.end(), tokens.begin())) {
					continue;
				}
				const std::span<const std::string> rest = tokens.subspan(key_tokens.size());
				if (rest.empty()) return is_tag_kind(field.kind);
				if (field.kind == desc::field_kind::array || field.kind == desc::field_kind::map) {
					if (rest.size() == 1) return is_tag_kind(field.element);
					if (field.element != desc::field_kind::structure) return false;
					const desc::type_schema* nested = descs.find_schema(field.ref_type);
					return nested && reference_field_at(descs, *nested, rest.subspan(1), depth + 1);
				}
				if (field.kind == desc::field_kind::structure) {
					const desc::type_schema* nested = descs.find_schema(field.ref_type);
					return nested && reference_field_at(descs, *nested, rest, depth + 1);
				}
				return false;
			}
		}
		return false;
	}
}

std::vector<edt::raw_reference> edt::collect_raw_references(const json::value& document)
{
	std::vector<raw_reference> out;
	collector{ out }.value(document, {}, {}, {}, 0);
	return out;
}

std::string_view edt::usage_kind_name(usage_kind kind)
{
	switch (kind) {
	case usage_kind::field: return "field";
	case usage_kind::parent: return "parent";
	case usage_kind::component: return "component";
	case usage_kind::loose: return "loose";
	case usage_kind::manifest: return "manifest";
	}
	return "loose";
}

std::optional<edt::usage_kind> edt::classify_reference(const raw_reference& reference, const desc::desc_system* descs)
{
	if (reference.place == reference_place::parent) return usage_kind::parent;
	if (reference.place == reference_place::component) return usage_kind::component;
	if (descs && !reference.owner_type.empty() && reference.json_pointer.starts_with(reference.owner_pointer)) {
		if (const desc::type_schema* schema = descs->find_schema(reference.owner_type)) {
			std::vector<std::string> tokens;
			const std::string_view inside = std::string_view(reference.json_pointer).substr(reference.owner_pointer.size());
			if (desc::json_pointer_tokens(inside, tokens) && reference_field_at(*descs, *schema, tokens, 0)) return usage_kind::field;
		}
	}
	if (reference.guid) return usage_kind::loose;
	const res::tag path{ reference.path };
	if (is_engine_protocol(path.protocol())) return usage_kind::loose;
	return std::nullopt;
}
