#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_screen_anchor.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// The engine's runtime components in the base catalog (HOT_RELOAD_PLAY.md section 4.3). Each schema is described here,
// from outside the component: no member, base or virtual function is added to it.

namespace scn
{
	BOOST_DESCRIBE_STRUCT(local_transform, (), (local))
	BOOST_DESCRIBE_STRUCT(name_component, (), (name))
	BOOST_DESCRIBE_STRUCT(node_id_component, (), (path))
	BOOST_DESCRIBE_STRUCT(parent_component, (), (parent))
	BOOST_DESCRIBE_STRUCT(children_component, (), (children))
	BOOST_DESCRIBE_STRUCT(object_component, (), ())
	BOOST_DESCRIBE_STRUCT(renderable, (), ())
	BOOST_DESCRIBE_STRUCT(scene_anchor_component, (), ())
	BOOST_DESCRIBE_STRUCT(directional_light, (), (direction, diffuse, ambient, specular))
	BOOST_DESCRIBE_STRUCT(mouse_controller_component, (), (movement_speed, rotating_speed, distance, rotation, position,
		last_mouse_pos, is_moving, is_rotating, input_enabled))
	BOOST_DESCRIBE_STRUCT(screen_anchor_component, (), (target, offset))

	BOOST_DESCRIBE_STRUCT(mesh_view, (), (vx_begin, vx_end, ind_begin, ind_end))
	BOOST_DESCRIBE_STRUCT(mesh_component, (), (mesh))
	BOOST_DESCRIBE_STRUCT(geometry_component, (), (geom_tag))
	// `dirty` is left out: a restored material is compiled again.
	BOOST_DESCRIBE_STRUCT(material_desc_component, (), (mlt_desc))
	BOOST_DESCRIBE_STRUCT(skeleton_component, (), (skinning_tag, bone_count, show_skeleton))
	BOOST_DESCRIBE_STRUCT(skinning_component, (), (skinning_tag, skeleton_entity))
	BOOST_DESCRIBE_STRUCT(bone_component, (), (offset, index))
	BOOST_DESCRIBE_STRUCT(sky_component, (), (cube_map))
	// The viewport is handed out by the Game view and `wireframe` is a debug view of the editor: neither is saved.
	BOOST_DESCRIBE_STRUCT(camera_component, (), (fov, near_distance, far_distance, layer, clear, projection, ortho_size,
		color_targets, depth_target, tp_accum_target, tp_reveal_target))
	BOOST_DESCRIBE_STRUCT(animation_controller_component, (), (current_animation, current_time, speed, playing, loop,
		duration, ticks_per_second))
	BOOST_DESCRIBE_STRUCT(animation_collection_component, (), (clips))
	BOOST_DESCRIBE_STRUCT(animated_node_component, (), (node_name))
	BOOST_DESCRIBE_STRUCT(prefab_instance, (), (source_prefab, local_overrides))
	BOOST_DESCRIBE_STRUCT(component_tracker, (), (tracked_components))

	// What the schema compares two trackers with; a free function, the struct stays as it is.
	inline bool operator==(const tracked_component_info& a, const tracked_component_info& b)
	{
		return a.type_name == b.type_name && a.source_desc == b.source_desc && a.local_overrides == b.local_overrides;
	}
}

namespace desc
{
	// A material tag, or an inline material written back inline (desc_ref_traits).
	template<>
	inline constexpr auto field_attrs<scn::material_desc_component> = attrs(
		f("mlt_desc").kind(field_kind::desc_ref).ref("material_desc"));

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
			scn::prefab_desc holder;
			holder.deserialize(*ctx.system, in.as_object());
			value = std::move(holder.get_root());
			return true;
		}
	};

	// Keyed by component name, written in name order so that two snapshots of the same world are equal.
	template<>
	struct json_codec<std::unordered_map<std::string, scn::tracked_component_info>>
	{
		using map = std::unordered_map<std::string, scn::tracked_component_info>;

		static json::value to_json(const map& tracked)
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

		static bool from_json(map& tracked, const json::value& in)
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
	};
}

namespace
{
	template<typename T>
	void add(scn::component_catalog& catalog, std::string_view name)
	{
		catalog.add<T>(std::string{ name }, desc::bind_described<T>(name, 1, desc::schema_kind::component));
	}
}

void scn::register_engine_components(component_catalog& catalog)
{
	add<local_transform>(catalog, "scn.local_transform");
	add<name_component>(catalog, "scn.name");
	add<node_id_component>(catalog, "scn.node_id");
	add<parent_component>(catalog, "scn.parent");
	add<children_component>(catalog, "scn.children");
	add<object_component>(catalog, "scn.object");
	add<renderable>(catalog, "scn.renderable");
	add<scene_anchor_component>(catalog, "scn.scene_anchor");
	add<directional_light>(catalog, "scn.directional_light");
	add<mouse_controller_component>(catalog, "scn.mouse_controller");
	add<screen_anchor_component>(catalog, "scn.screen_anchor");

	catalog.add_derived<world_transform>("scn.world_transform");
	catalog.add_derived<depth_level>("scn.depth_level");
	catalog.add_derived<screen_anchor_result>("scn.screen_anchor_result");
	catalog.add_derived<bone_matrices_component>("scn.bone_matrices");

	add<mesh_component>(catalog, "scn.mesh");
	add<geometry_component>(catalog, "scn.geometry");
	add<material_desc_component>(catalog, "scn.material_desc");
	add<skeleton_component>(catalog, "scn.skeleton");
	add<skinning_component>(catalog, "scn.skinning");
	add<bone_component>(catalog, "scn.bone");
	add<sky_component>(catalog, "scn.sky");
	add<camera_component>(catalog, "scn.camera");
	add<animation_controller_component>(catalog, "scn.animation_controller");
	add<animation_collection_component>(catalog, "scn.animation_collection");
	add<animated_node_component>(catalog, "scn.animated_node");
	add<prefab_instance>(catalog, "scn.prefab_instance");
	add<component_tracker>(catalog, "scn.component_tracker");
}
