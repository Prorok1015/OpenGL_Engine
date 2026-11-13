#include "scn_engine_components_detail.h"
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// The animation and prefab components of the engine catalog (see scn_engine_components.cpp).

namespace scn
{
	BOOST_DESCRIBE_STRUCT(animation_controller_component, (), (current_animation, current_time, speed, playing, loop,
		duration, ticks_per_second))
	BOOST_DESCRIBE_STRUCT(animation_collection_component, (), (clips))
	BOOST_DESCRIBE_STRUCT(animated_node_component, (), (node_name))
	BOOST_DESCRIBE_STRUCT(prefab_instance, (), (source_prefab, local_overrides))
	BOOST_DESCRIBE_STRUCT(component_tracker, (), (tracked_components))
}

namespace desc
{
	// The elements of a vector of handles are custom fields: they go through the same tag-or-inline rule as one handle.
	template<>
	struct json_codec<res::res_handle<scn::animation_clip_desc>>
	{
		using handle = res::res_handle<scn::animation_clip_desc>;
		static json::value to_json(const handle& clip) { return desc_ref_traits<handle>::to_json(clip); }
		static bool from_json(handle& clip, const json::value& in, const read_context& ctx)
		{
			return desc_ref_traits<handle>::from_json(clip, in, ctx);
		}
	};

	// The node a prefab instance remembers, written the way a prefab desc writes its root (the descs of module types
	// become JSON there, before the module goes). Spawning leaves it empty (assemble_prefab), which is written as null.
	template<>
	struct json_codec<scn::prefab_desc::prefab_node>
	{
		using node = scn::prefab_desc::prefab_node;

		static json::value to_json(const node& value)
		{
			if (value.components.empty() && value.children.empty() && value.overrides.empty()) {
				return json::value(nullptr);
			}
			scn::prefab_desc holder;
			holder.get_root() = value;
			json::object out;
			holder.serialize(out);
			// A prefab desc does not write the name of its root; the remembered node has one.
			out["__root_name"] = value.name;
			return json::value(std::move(out));
		}

		static bool from_json(node& value, const json::value& in, const read_context& ctx)
		{
			if (in.is_null()) {
				value = node{};
				return true;
			}
			if (!in.is_object() || !ctx.system) {
				return false;
			}
			json::object data = in.as_object();
			std::string name;
			if (const auto* root_name = data.if_contains("__root_name"); root_name && root_name->is_string()) {
				name = std::string{ root_name->as_string() };
			}
			data.erase("__root_name");
			scn::prefab_desc holder;
			holder.deserialize(*ctx.system, data);
			value = std::move(holder.get_root());
			value.name = std::move(name);
			return true;
		}
	};
}

// Keyed by component name, written in name order so that two snapshots of the same world are equal.
json::value desc::json_codec<std::unordered_map<std::string, scn::tracked_component_info>>::to_json(const map& tracked)
{
	std::vector<const map::value_type*> entries;
	entries.reserve(tracked.size());
	for (const auto& entry : tracked) {
		entries.push_back(&entry);
	}
	std::ranges::sort(entries, {}, [](const map::value_type* entry) { return entry->first; });
	json::object out;
	for (const auto* entry : entries) {
		json::object info;
		info["type_name"] = entry->second.type_name;
		info["source_desc"] = entry->second.source_desc.view();
		info["local_overrides"] = entry->second.local_overrides;
		out[entry->first] = std::move(info);
	}
	return json::value(std::move(out));
}

bool desc::json_codec<std::unordered_map<std::string, scn::tracked_component_info>>::from_json(map& tracked, const json::value& in)
{
	if (!in.is_object()) {
		return false;
	}
	map parsed;
	for (const auto& [name, value] : in.as_object()) {
		const json::object* info = value.if_object();
		if (!info) {
			return false;
		}
		scn::tracked_component_info item;
		if (const auto* type = info->if_contains("type_name"); type && type->is_string()) {
			item.type_name = std::string{ type->as_string() };
		}
		if (const auto* source = info->if_contains("source_desc"); source && source->is_string() && !source->as_string().empty()) {
			item.source_desc = res::tag{ std::string_view{ source->as_string() } };
		}
		if (const auto* overrides = info->if_contains("local_overrides"); overrides && overrides->is_object()) {
			item.local_overrides = overrides->as_object();
		}
		parsed.emplace(std::string{ name }, std::move(item));
	}
	tracked = std::move(parsed);
	return true;
}

void scn::detail::register_animation_components(component_catalog& catalog)
{
	add_engine_component<animation_controller_component>(catalog, "scn.animation_controller");
	add_engine_component<animation_collection_component>(catalog, "scn.animation_collection");
	add_engine_component<animated_node_component>(catalog, "scn.animated_node");
	add_engine_component<prefab_instance>(catalog, "scn.prefab_instance");
	add_engine_component<component_tracker>(catalog, "scn.component_tracker");
}
