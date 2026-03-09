#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

#include <unordered_map>
#include <string>

namespace scn {
	struct animation_node;

	class keyframes_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "keyframes_desc";

		using base_type = desc::desc_base;

		keyframes_desc() = default;
		~keyframes_desc() = default;
		keyframes_desc(const keyframes_desc&) = default;
		keyframes_desc(keyframes_desc&&) = default;
		keyframes_desc& operator=(const keyframes_desc&) = default;
		keyframes_desc& operator=(keyframes_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			keyframes_desc& other_desc = static_cast<keyframes_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		std::unordered_map<std::string, scn::animation_node> keyframes;
	};

	void assemble_keyframes(entt::registry& reg, entt::entity e, const keyframes_desc& desc, std::string_view name);
}
