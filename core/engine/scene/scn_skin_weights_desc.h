#pragma once
#include "desc_base.hpp"
#include "res_tag.h"
#include "ecs_system.h"
#include <vector>

namespace scn {
	// Stores per-vertex skinning weights. Registered with skinning_manager during assembly.
	// Each vertex column: [bone_idx_0, ..., bone_idx_N, weight_0_bits, ..., weight_N_bits]
	// where weight bits are float encoded via std::bit_cast<uint32_t>(float).
	//
	// JSON format:
	//   {
	//     "__type": "skin_weights_desc",
	//     "tag": "memory://objects/model.desc",
	//     "weights": [ [1, 2, 0x3F800000, 0x3F000000], ... ]
	//   }
	class skin_weights_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "skin_weights_desc";

		using base_type = desc::desc_base;

		skin_weights_desc() = default;
		~skin_weights_desc() = default;
		skin_weights_desc(const skin_weights_desc&) = default;
		skin_weights_desc(skin_weights_desc&&) = default;
		skin_weights_desc& operator=(const skin_weights_desc&) = default;
		skin_weights_desc& operator=(skin_weights_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			skin_weights_desc& other_desc = static_cast<skin_weights_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		res::tag registration_tag;
		std::vector<std::vector<uint32_t>> weights;
	};

	void assemble_skin_weights(entt::registry& reg, entt::entity e, const skin_weights_desc& desc, std::string_view name);
}
