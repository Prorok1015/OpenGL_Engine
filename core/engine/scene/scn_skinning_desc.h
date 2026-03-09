#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

namespace scn {
	// Adds skinning data to a mesh entity: skinning_component (always) +
	// bone_matrices_component initialized to identity matrices (when bone_count > 0).
	// JSON format:
	//   {
	//     "__type": "skinning_desc",
	//     "bone_count": 4
	//   }
	class skinning_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "skinning_desc";

		using base_type = desc::desc_base;

		skinning_desc() = default;
		~skinning_desc() = default;
		skinning_desc(const skinning_desc&) = default;
		skinning_desc(skinning_desc&&) = default;
		skinning_desc& operator=(const skinning_desc&) = default;
		skinning_desc& operator=(skinning_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			skinning_desc& other_desc = static_cast<skinning_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		int bone_count = 0;
	};

	void assemble_skinning(entt::registry& reg, entt::entity e, const skinning_desc& desc, std::string_view name);
}
