#pragma once
#include "desc_base.hpp"
#include "scn_material_desc.h"
#include "geom/rnd_geometry_desc.h"

namespace scn {
	// Describes a renderable mesh node for use within prefab hierarchies.
	// Replaces what prototype_desc::node_t with mesh does, but as a reusable prefab component.
	// JSON format:
	//   {
	//     "__type": "mesh_node_desc",
	//     "geometry": "res://some.desc",
	//     "vx_begin": 0,
	//     "vx_end": 24,
	//     "ind_begin": 0,
	//     "ind_end": 36,
	//     "material": "res://material.desc"
	//   }
	class mesh_node_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "mesh_node_desc";

		using base_type = desc::desc_base;

		mesh_node_desc() = default;
		~mesh_node_desc() = default;
		mesh_node_desc(const mesh_node_desc&) = default;
		mesh_node_desc(mesh_node_desc&&) = default;
		mesh_node_desc& operator=(const mesh_node_desc&) = default;
		mesh_node_desc& operator=(mesh_node_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			mesh_node_desc& other_desc = static_cast<mesh_node_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		res::res_handle<rnd::geometry_desc> geometry;
		std::size_t vx_begin = 0;
		std::size_t vx_end = 0;
		std::size_t ind_begin = 0;
		std::size_t ind_end = 0;
		res::res_handle<scn::material_desc> material;
	};

	void assemble_mesh_node(entt::registry& reg, entt::entity e, const mesh_node_desc& desc, const std::string_view name);
}
