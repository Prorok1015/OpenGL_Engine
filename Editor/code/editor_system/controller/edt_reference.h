#pragma once
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// The two forms of a reference in a desc file (EPIC-57 section 4.4): a tag string, or {"guid": ..., "path": ...}. Every
// check of the editor reads both the same way.
namespace edt
{
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
}
