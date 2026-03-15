#pragma once
#include "scn_model.h"
#include "scn_camera_component.hpp"
#include "scn_glm_json_convert.h"
#include "eng_transform_3d.hpp"
#include "level/scn_prefab_desc.h"
#include <entt/entt.hpp>
#include <boost/json.hpp>
#include <string>
#include <vector>

// Converts the current live ECS state back into a level_desc JSON object.
// Supports: prefab instances (skin_prototype_desc), cameras, directional lights,
// and plain entities with transforms.  Internal prefab children (bones, meshes)
// are NOT recursed — only the root entity referencing the prefab is saved.

namespace edt
{
	namespace json = boost::json;

	// ─── helpers ──────────────────────────────────────────────────────────────

	inline json::object serialize_transform(const scn::local_transform& lt)
	{
		json::object t;
		eng::transform3d tr{ lt.local };

		const glm::vec3 pos     = tr.get_pos();
		const glm::vec3 rot_deg = tr.get_angles_degrees();
		const glm::vec3 scale   = tr.get_scale();

		if (pos     != glm::vec3{ 0.f }) t["position"] = json::value_from(pos);
		if (rot_deg != glm::vec3{ 0.f }) t["rotation"] = json::value_from(rot_deg);
		if (scale   != glm::vec3{ 1.f }) t["scale"]    = json::value_from(scale);

		return t;
	}

	// Recursively serialize one entity.
	// For entities with prefab_instance, children are NOT visited (they belong to the asset).
	inline json::value serialize_entity(entt::registry& reg, entt::entity ent)
	{
		json::object node;

		// ── transform ───────────────────────────────────────────────────────
		if (reg.all_of<scn::local_transform>(ent)) {
			json::object t = serialize_transform(reg.get<scn::local_transform>(ent));
			if (!t.empty())
				node["transform"] = std::move(t);
		}

		// ── prefab instance — serialize as asset reference, skip children ──
		if (reg.all_of<scn::prefab_instance>(ent)) {
			const auto& inst = reg.get<scn::prefab_instance>(ent);

			// Key name for the component entry
			std::string comp_key = "prototype";
			if (reg.all_of<scn::name_component>(ent))
				comp_key = reg.get<scn::name_component>(ent).name;

			json::object comp;
			comp["__type"]   = "skin_prototype_desc";
			comp["__parent"] = inst.source_prefab.string();

			json::object components;
			components[comp_key] = std::move(comp);
			node["components"] = std::move(components);

			return node; // don't recurse internal prefab children
		}

		// ── manually assembled / manually created entity ───────────────────
		json::object components;

		if (reg.all_of<scn::camera_component>(ent)) {
			const auto& cam = reg.get<scn::camera_component>(ent);
			json::object c;
			c["__type"] = "camera_desc";
			// Persist non-default values so reloading restores them
			if (cam.fov           != 45.f)    c["fov"]  = cam.fov;
			if (cam.near_distance != 0.1f)    c["near"] = cam.near_distance;
			if (cam.far_distance  != 1000.f)  c["far"]  = cam.far_distance;
			components["camera"] = std::move(c);
		}

		if (reg.all_of<scn::directional_light>(ent)) {
			json::object c;
			c["__type"] = "directional_light_desc";
			components["directional_light"] = std::move(c);
		}

		if (!components.empty())
			node["components"] = std::move(components);

		// ── recurse children ─────────────────────────────────────────────────
		if (reg.all_of<scn::children_component>(ent)) {
			json::object children_obj;
			for (entt::entity child : reg.get<scn::children_component>(ent).children) {
				if (!reg.valid(child)) continue;

				std::string name = "entity_" +
					std::to_string(static_cast<uint32_t>(entt::to_integral(child)));
				if (reg.all_of<scn::name_component>(child))
					name = reg.get<scn::name_component>(child).name;

				children_obj[name] = serialize_entity(reg, child);
			}
			if (!children_obj.empty())
				node["children"] = std::move(children_obj);
		}

		return node;
	}

	// ─── public entry point ───────────────────────────────────────────────────

	// Build the full level_desc JSON from the current scene state.
	// Call with the registry of the "3d_scene" world.
	inline json::object serialize_level(
		entt::registry&                  reg,
		const std::string&               level_name,
		const std::string&               world_name,
		const std::vector<std::string>&  systems)
	{
		// Collect children of every scene anchor
		json::object children_obj;
		for (auto anchor : reg.view<scn::scene_anchor_component>()) {
			if (!reg.all_of<scn::children_component>(anchor))
				continue;
			for (entt::entity child : reg.get<scn::children_component>(anchor).children) {
				if (!reg.valid(child)) continue;

				std::string name = "entity_" +
					std::to_string(static_cast<uint32_t>(entt::to_integral(child)));
				if (reg.all_of<scn::name_component>(child))
					name = reg.get<scn::name_component>(child).name;

				children_obj[name] = serialize_entity(reg, child);
			}
		}

		// Build systems array
		json::array systems_arr;
		for (const auto& s : systems)
			systems_arr.push_back(json::value_from(s));

		// world_desc node
		json::object world_obj;
		world_obj["__type"]      = "world_desc";
		world_obj["name"]        = world_name;
		world_obj["components"]  = json::object{{ "anchor", json::object{{"__type", "anchor_desc"}} }};
		world_obj["children"]    = std::move(children_obj);
		world_obj["systems"]     = std::move(systems_arr);

		// level_desc root
		json::object level_obj;
		level_obj["__type"]  = "level_desc";
		level_obj["name"]    = level_name;
		level_obj["worlds"]  = json::array{ std::move(world_obj) };

		return level_obj;
	}
}
