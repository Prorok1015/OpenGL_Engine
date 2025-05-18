#include "scn_prototype_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"
#include "geom/rnd_geometry_desc.h"

namespace {
	struct context
	{
		desc::desc_system& desc_system;
		desc::desc_base& self;
	};
}

namespace scn
{
	void tag_invoke(json::value_from_tag, json::value& out, const scn::prototype_desc::node_t& c)
	{
		json::object object;
		object["name"] = c.name;
		object["local"] = json::value_from(c.local);
		if (c.mesh.has_value()) {
			auto& mesh = c.mesh.value();
			json::object mesh_obj;
			mesh_obj["vx_begin"] = mesh.vx_begin;
			mesh_obj["vx_end"] = mesh.vx_end;
			mesh_obj["ind_begin"] = mesh.ind_begin;
			mesh_obj["ind_end"] = mesh.ind_end;
			mesh_obj["material"] = json::value_from(mesh.material->get_tag());
			object["mesh"] = mesh_obj;
		}

		if (!c.children.empty()) {
			json::array children;
			for (auto& child : c.children) {
				children.push_back(json::value_from(child));
			}
			object["children"] = children;
		}
	}

	scn::prototype_desc::node_t tag_invoke(json::value_to_tag<scn::prototype_desc::node_t>, const json::value& data, const context& ctx)
	{
		prototype_desc::node_t node;
		if (data.is_object())
		{
			auto obj = data.get_object();
			if (obj.contains("name")) {
				node.name = json::value_to<std::string>(obj.at("name"));
			}
			if (obj.contains("local")) {
				node.local = json::value_to<glm::mat4>(obj.at("local"));
			}
			if (obj.contains("mesh")) {
				auto mesh_obj = obj.at("mesh").get_object();
				node.mesh = prototype_desc::mesh_t{};
				node.mesh->vx_begin = json::value_to<std::size_t>(mesh_obj.at("vx_begin"));
				node.mesh->vx_end = json::value_to<std::size_t>(mesh_obj.at("vx_end"));
				node.mesh->ind_begin = json::value_to<std::size_t>(mesh_obj.at("ind_begin"));
				node.mesh->ind_end = json::value_to<std::size_t>(mesh_obj.at("ind_end"));

				if (mesh_obj.contains("material")) {
					node.mesh->material = ctx.desc_system.get_or_override_desc<scn::material_desc>(ctx.self, mesh_obj.at("material"));
				}
			}
			if (obj.contains("children")) {
				auto children_array = obj.at("children").get_array();
				for (auto& child : children_array) {
					node.children.push_back(json::value_to<prototype_desc::node_t>(child, ctx));
				}
			}
		}

		return node;
	}
}

void scn::prototype_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("geometry")) {
		geometry = desc_system.get_or_override_desc<rnd::geometry_desc>(*this, data.at("geometry"));
	}

	if (data.contains("tree")) {
		root = json::value_to<node_t>(data.at("tree"), context{ desc_system, *this });
	}
}

void scn::prototype_desc::serialize(const res::tag& tag, res::resource_system& res_system, json::object& data) const
{
	/*
		"geometry": "res://cube.desc",
		"tree": {
			"name": "Cube",
			"local": {
				"position": [0, 0, 0],
				"rotation": [0, 0, 0],
				"scale": [1, 1, 1]
			},
			"children": [
				{
					"name": "Child1",
					"local": {
						"position": [1, 0, 0],
						"rotation": [0, 0, 0],
						"scale": [1, 1, 1]
					}
					"mesh": {
						"vx_begin": 0,
						"vx_end": 100,
						"ind_begin": 0,
						"ind_end": 100,
						"material": "res://material.desc"
					}
				},
				{
					"name": "Child2",
					"local": {
						"position": [-1, 0, 0],
						"rotation": [0, 0, 0],
						"scale": [1, 1, 1]
					}
				}
			]
		}
	*/
	
	data["geometry"] = json::value_from(geometry->get_tag());
	data["tree"] = json::value_from(root);
}

void scn::prototype_desc::load_prototype(entt::registry& registry, entt::entity parent)
{
	load_prototype_node(registry, parent, root);
}

entt::entity scn::prototype_desc::load_prototype_node(entt::registry& registry, entt::entity parent, const node_t& node)
{	
	auto ent = registry.create();
	registry.emplace<scn::name_component>(ent, node.name);
	registry.emplace<scn::local_transform>(ent, node.local);
	registry.emplace<scn::parent_component>(ent, parent);
	registry.emplace<scn::renderable>(ent);

	if (registry.all_of<scn::children_component>(parent)) {
		auto& children = registry.get<scn::children_component>(parent);
		auto& children_list = children.children;
		children_list.push_back(ent);
	} else {
		registry.emplace<scn::children_component>(parent, std::vector<entt::entity>{ ent });
	}

	if (node.mesh.has_value()) {
		auto& mesh = node.mesh.value();
		res::mesh_view mesh_data;
		mesh_data.vx_begin = mesh.vx_begin;
		mesh_data.vx_end = mesh.vx_end;
		mesh_data.ind_begin = mesh.ind_begin;
		mesh_data.ind_end = mesh.ind_end;
		registry.emplace<scn::mesh_component>(ent, scn::mesh_component{ .mesh = mesh_data });
		registry.emplace<scn::geometry_component>(ent, scn::geometry_component{ .geom_tag = geometry->get_tag() });
		registry.emplace<scn::material_desc_component>(ent, scn::material_desc_component{ .mlt_desc = mesh.material });
	}

	for (auto& child : node.children) {
		load_prototype_node( registry, ent, child);
	}

	return ent;
}
