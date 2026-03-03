#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

namespace scn {
	class directional_light_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;
		directional_light_desc() = default;
		~directional_light_desc() = default;
		directional_light_desc(const directional_light_desc&) = default;
		directional_light_desc(directional_light_desc&&) = default;
		directional_light_desc& operator=(const directional_light_desc&) = default;
		directional_light_desc& operator=(directional_light_desc&&) = default;
		virtual void copy_to(desc_base& other) const override {
			directional_light_desc& other_desc = static_cast<directional_light_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		glm::vec4 direction{ -0.2f, -1.0f, -0.3f, 0.0 };
		glm::vec4 diffuse{ 0.5f, 0.5f, 0.5f, 1.f };
		glm::vec4 ambient{ 0.2f, 0.2f, 0.2f, 1.f };
		glm::vec4 specular{ 1 };
	};

	void assemble_directional_light(entt::registry& reg, entt::entity e, const directional_light_desc& desc, const std::string_view name);
}