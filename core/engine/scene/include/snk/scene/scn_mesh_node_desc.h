#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <entt/fwd.hpp> // entt::registry / entt::entity in assemble_mesh_node decl (was pulled in
                       // transitively via scn_material_desc.h before it was POD-ified, EPIC-49 US-49-5)
#include <snk/scene/scn_material_desc.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <boost/describe.hpp>
#include <snk/common/snk_api.h>

namespace scn {
	class mesh_node_desc;
}

// The schema of a mesh node (#720): what component.set checks for an agent and what the pre-Play check reads. Declared
// before the class so that the serializer generated for schema_desc sees the attributes. The keys, their order and when
// each is written are those of the file format below: a resaved node of an imported model keeps its bytes.
namespace desc {
	template<>
	inline constexpr auto field_attrs<scn::mesh_node_desc> = attrs(
		f("geometry").kind(field_kind::desc_ref).ref("geometry_desc").write_if_valid().tooltip("Geometry of the mesh: a geometry desc tag, or an inline geometry"),
		f("vx_begin").tooltip("First vertex of the range drawn"),
		f("vx_end").tooltip("End of the vertex range (exclusive)"),
		f("ind_begin").tooltip("First index of the range drawn"),
		f("ind_end").tooltip("End of the index range (exclusive)"),
		f("material").kind(field_kind::desc_ref).ref("material_desc").write_if_valid().tooltip("Material of the mesh: a material desc tag, or an inline material"),
		f("visible").write_if_not_default().tooltip("Drawn (#560): a hidden mesh stays in the hierarchy"));
}

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
	//     "material": "res://material.desc",
	//     "visible": false              // only when hidden: absent means visible (#560)
	//   }
	class SNK_API mesh_node_desc : public desc::schema_desc<mesh_node_desc>
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
			copy_to_same(*this, other);
		}
		// Defined in the engine and kept there although the schema does the work: modules built for 3.15 import them (#720).
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		res::res_handle<rnd::geometry_desc> geometry;
		std::size_t vx_begin = 0;
		std::size_t vx_end = 0;
		std::size_t ind_begin = 0;
		std::size_t ind_end = 0;
		res::res_handle<scn::material_desc> material;
		bool visible = true;

		BOOST_DESCRIBE_CLASS(mesh_node_desc, (), (geometry, vx_begin, vx_end, ind_begin, ind_end, material, visible), (), ())
	};

	SNK_API void assemble_mesh_node(entt::registry& reg, entt::entity e, const mesh_node_desc& desc, const std::string_view name);
}
