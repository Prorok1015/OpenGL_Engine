#pragma once
#include "common.h"
#include "scn_prototype_desc.h"

namespace scn
{
	class animatable_prototype_desc : public prototype_desc
	{
	public:
		animatable_prototype_desc() = default;
		virtual ~animatable_prototype_desc() = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			animatable_prototype_desc& other_desc = static_cast<animatable_prototype_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		template<class T>
		struct keyframe_t
		{
			T value;
			float time;
		};

		using position_keyframe = keyframe_t<glm::vec3>;
		using rotation_keyframe = keyframe_t<glm::quat>;
		using scale_keyframe = keyframe_t<glm::vec3>;

		struct animatable_node_t
		{
			std::string name;
			std::vector<position_keyframe> pos_keyframes;
			std::vector<rotation_keyframe> rotate_keyframes;
			std::vector<scale_keyframe> scale_keyframes;
		};

		struct animation_t
		{
			std::string name = "unknown animation";
			float duration = 0.f;
			float ticks_per_second = 0.f;
			std::unordered_map<std::string, animatable_node_t> nodes;
		};

	protected:
		void load_prototype_animations(entt::registry& registry, entt::entity root) const;
		virtual entt::entity load_prototype(entt::registry& registry, entt::entity parent) const override;
		virtual entt::entity load_prototype_node(entt::registry& registry, entt::entity parent, const node_t& node, load_context& ctx) const override;

		using animation_name = std::string;
		std::unordered_map<animation_name, animation_t> animations;
	};
}