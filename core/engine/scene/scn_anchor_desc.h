#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"
#include <glm/glm.hpp>

namespace scn {
	class scene_anchor_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;
		scene_anchor_desc() = default;
		~scene_anchor_desc() = default;
		scene_anchor_desc(const scene_anchor_desc&) = default;
		scene_anchor_desc(scene_anchor_desc&&) = default;
		scene_anchor_desc& operator=(const scene_anchor_desc&) = default;
		scene_anchor_desc& operator=(scene_anchor_desc&&) = default;
		virtual void copy_to(desc_base& other) const override {
			scene_anchor_desc& other_desc = static_cast<scene_anchor_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override {};
		virtual void serialize(json::object&) const override {};
	};

	void assemble_scene_anchor(entt::registry& reg, entt::entity e, const scene_anchor_desc& desc, const std::string_view name);
}