#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "scn_material_desc.h"
#include "geom/rnd_geometry_desc.h"
#include <optional>

namespace scn
{
	class prototype_desc : public desc::desc_base
	{
	public:
		prototype_desc() = default;
		virtual ~prototype_desc() = default;

		virtual void copy_to(desc_base& other) const override
		{
			prototype_desc& other_desc = static_cast<prototype_desc&>(other);
			other_desc = *this;
		}

		struct mesh_t
		{
			std::size_t vx_begin = 0;
			std::size_t vx_end = 0;
			std::size_t ind_begin = 0;
			std::size_t ind_end = 0;
			std::shared_ptr<scn::material_desc> material;
		};

		struct node_t
		{
			std::string name = "unknown";
			glm::mat4 local{ 1.0 };
			std::vector<node_t> children;
			std::optional<mesh_t> mesh;
		};

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		virtual void load_prototype(entt::registry& registry, entt::entity parent);

	protected:
		virtual entt::entity load_prototype_node(entt::registry& registry, entt::entity parent, const node_t& node);

	public:
		node_t root;
		std::shared_ptr<rnd::geometry_desc> geometry;
	};
}
