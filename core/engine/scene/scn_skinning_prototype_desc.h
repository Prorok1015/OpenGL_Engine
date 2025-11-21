#pragma once
#include "common.h"
#include "scn_animatable_prototype_desc.h"
#include "texture/rnd_texture_desc.h"

namespace scn
{
	class skinning_prototype_desc : public animatable_prototype_desc
	{
	public:
		struct weight
		{
			std::string bname;
			float bweight = 0.f;
		};

		struct bones_influence
		{
			std::string mesh_name;
			std::vector<std::vector<weight>> vertex_weights;
		};

		skinning_prototype_desc() = default;
		virtual ~skinning_prototype_desc() = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			skinning_prototype_desc& other_desc = static_cast<skinning_prototype_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
		virtual void load_prototype(entt::registry& registry, entt::entity parent) override;

		std::vector<std::vector<uint32_t>> get_2d_array_bonesids_weights() const;

	private:
		entt::entity load_prototype_node(entt::registry& registry, entt::entity parent, const node_t& node, load_context& ctx) const override;

		void deserialize_bones(const json::object& data);
		void serialize_bones(json::object& data) const;

		std::unordered_map<std::string, std::pair<glm::mat4, int>> bones_offsets;
		std::vector<bones_influence> bones_weights;
		int bone_count = 0;
	};
}