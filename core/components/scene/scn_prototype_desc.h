#pragma once
#include "common.h"
#include "desc_base.hpp"
#include "scn_material_desc.h"


namespace scn
{
	class prototype_desc : public desc::desc_base
	{
	public:
		struct mesh_t
		{
			std::size_t vx_begin = 0;
			std::size_t vx_end = 0;
			std::size_t ind_begin = 0;
			std::size_t ind_end = 0;

			res::tag material_tag;
		};

		struct node_t
		{
			std::string name;
			glm::mat4 local;
			std::vector<node_t> children;
			std::optional<mesh_t> mesh;
		};

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const override;

		virtual void load_prototype(desc::desc_system& desc_system, entt::registry& registry, entt::entity parent);

	private:
		void load_prototype_node(desc::desc_system& desc_system, entt::registry& registry, entt::entity parent, const node_t& node);

	public:
		node_t root;
		res::tag geometry_tag;
	};
}
