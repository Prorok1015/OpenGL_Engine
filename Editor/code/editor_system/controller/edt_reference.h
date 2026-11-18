#pragma once
#include <snk/model_codec/res_asset_id.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/model_codec/res_reference.h>
#include <snk/resource/res_system.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json.hpp>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// The two forms of a reference in a desc file (EPIC-57 section 4.4): a tag string, or {"guid": ..., "path": ...}. Every
// check of the editor reads both the same way.
namespace edt
{
	// The engine's own protocols: a string naming one is meant as a reference even where no schema says so.
	inline bool is_engine_protocol(std::string_view protocol)
	{
		static constexpr std::string_view k_protocols[] = { "res", "engine", "memory", "user", "edt", "code", "cache", "mod" };
		for (const std::string_view known : k_protocols) {
			if (known == protocol) return true;
		}
		return false;
	}

	// An object where a reference may stand that is no inline desc (that one has "__type"): a reference object, good or
	// not (reference_object_problem says which).
	inline bool is_reference_object(const boost::json::value& value)
	{
		const boost::json::object* object = value.if_object();
		return object && !object->contains("__type");
	}

	// What is wrong with a reference object, empty when nothing: only "guid" and "path", at least one of them, "guid" a
	// UUID that is not nil, "path" a string. The same rule as res::reference_from_json, with the reason.
	inline std::string reference_object_problem(const boost::json::object& object)
	{
		for (const auto& member : object) {
			const std::string_view key(member.key().data(), member.key().size());
			if (key != "guid" && key != "path") return std::format("a reference has only \"guid\" and \"path\", not \"{}\"", key);
		}
		const boost::json::value* guid = object.if_contains("guid");
		const boost::json::value* path = object.if_contains("path");
		if (!guid && !path) return "a reference needs a \"guid\" or a \"path\"";
		if (guid) {
			const std::optional<res::asset_id> id = guid->is_string()
				? res::asset_id_from_text(std::string_view(guid->as_string().data(), guid->as_string().size())) : std::nullopt;
			if (!id || id->is_nil()) return "\"guid\" must be a UUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) that is not nil";
		}
		if (path && !path->is_string()) return "\"path\" must be a string";
		return {};
	}

	// The text of the path a reference object holds; empty when it has none.
	inline std::string reference_path_text(const boost::json::object& object)
	{
		const boost::json::value* path = object.if_contains("path");
		return path && path->is_string() ? std::string(path->as_string().data(), path->as_string().size()) : std::string{};
	}

	// Where a reference object leads, as resource_system::resolve_reference loads it, and how its guid stands.
	struct reference_target
	{
		res::reference reference;
		// What it loads: the file that holds the guid, else its path. Empty: nothing.
		res::tag tag;
		// The asset index does not hold the guid: the path is what loads (or nothing, without one).
		bool unknown_id = false;
		// The index holds the guid under another path than the one written: the file moved since; `tag` is where it is.
		bool stale_path = false;
	};

	// Null `resources`, or one without an asset index: the guid cannot be looked up, so the path is what loads, as in a
	// game without an index, and nothing is said about the guid.
	inline std::optional<reference_target> target_of(const boost::json::value& value, const res::resource_system* resources)
	{
		const std::optional<res::reference> read = res::reference_from_json(value);
		if (!read) return std::nullopt;
		reference_target out{ *read, read->path };
		if (read->guid.is_nil() || !resources || !resources->has_asset_index()) return out;
		const std::optional<res::tag> held = resources->asset_path_of(read->guid);
		if (!held) {
			out.unknown_id = true;
			return out;
		}
		out.stale_path = !read->path.view().empty() && read->path.view() != held->view();
		out.tag = *held;
		return out;
	}

	// An object that can only be a reference with a guid: no key but "guid" and "path", and a "guid" that is a UUID that
	// is not nil. Any other "guid" is plain data: res::reference_from_json does not read it as a reference either.
	inline bool is_guid_reference(const boost::json::object& object)
	{
		for (const auto& member : object) {
			const std::string_view key(member.key().data(), member.key().size());
			if (key != "guid" && key != "path") return false;
		}
		const boost::json::value* guid = object.if_contains("guid");
		if (!guid || !guid->is_string()) return false;
		const std::optional<res::asset_id> id = res::asset_id_from_text(std::string_view(guid->as_string().data(), guid->as_string().size()));
		return id && !id->is_nil();
	}

	// The form an agent may write (#954), empty when fine: reference_object_problem, and no guid without a path.
	inline std::string agent_reference_form_problem(const boost::json::object& object)
	{
		if (std::string form = reference_object_problem(object); !form.empty()) return form;
		if (object.contains("guid") && reference_path_text(object).empty()) {
			return "a reference needs a \"path\": a \"guid\" alone is not accepted here";
		}
		return {};
	}

	// Empty unless the asset index holds the guid of a well-formed reference for another file than its path (#954). Asked
	// after the path's own check, so a bad path is named as one.
	inline std::string agent_reference_guid_problem(const boost::json::object& object, const res::resource_system* resources)
	{
		if (!object.contains("guid")) return {};
		const std::optional<reference_target> target = target_of(boost::json::value(object), resources);
		if (!target || !target->stale_path) return {};
		// As the asset database pairs names: letter case does not tell two files apart where the file system does not.
		const bool folded = res::asset_names_case_insensitive;
		if (res::asset_name_key(target->reference.path.view(), folded) == res::asset_name_key(target->tag.view(), folded)) return {};
		return std::format("\"guid\" and \"path\" name different files: {} is the ID of '{}'", res::to_text(target->reference.guid),
			target->tag.view());
	}
}
