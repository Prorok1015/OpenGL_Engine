#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

namespace scn {
	// Marks a node as a bone, adding bone_component with offset matrix and index.
	// JSON format:
	//   {
	//     "__type": "bone_desc",
	//     "offset_matrix": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
	//     "index": 0
	//   }
	class bone_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "bone_desc";

		using base_type = desc::desc_base;

		bone_desc() = default;
		~bone_desc() = default;
		bone_desc(const bone_desc&) = default;
		bone_desc(bone_desc&&) = default;
		bone_desc& operator=(const bone_desc&) = default;
		bone_desc& operator=(bone_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			bone_desc& other_desc = static_cast<bone_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		glm::mat4 offset_matrix{ 1.0f };
		int index = -1;
	};

	void assemble_bone(entt::registry& reg, entt::entity e, const bone_desc& desc, std::string_view name);
}
