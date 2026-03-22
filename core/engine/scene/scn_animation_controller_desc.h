#pragma once
#include "desc_base.hpp"
#include "ecs_system.h"
#include <string>

namespace scn {

class animation_controller_desc : public desc::desc_base {
public:
	static constexpr std::string_view __type = "animation_controller_desc";

	using base_type = desc::desc_base;

	animation_controller_desc() = default;
	~animation_controller_desc() = default;
	animation_controller_desc(const animation_controller_desc&) = default;
	animation_controller_desc(animation_controller_desc&&) = default;
	animation_controller_desc& operator=(const animation_controller_desc&) = default;
	animation_controller_desc& operator=(animation_controller_desc&&) = default;

	virtual void copy_to(desc_base& other) const override
	{
		animation_controller_desc& other_desc = static_cast<animation_controller_desc&>(other);
		other_desc = *this;
	}
	virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
	virtual void serialize(json::object&) const override;

	std::string default_animation;
	float speed = 1.f;
	bool autoplay = true;
	bool loop = true;
};

void assemble_animation_controller(entt::registry& reg, entt::entity e,
                                   const animation_controller_desc& desc, std::string_view name);
} // namespace scn
