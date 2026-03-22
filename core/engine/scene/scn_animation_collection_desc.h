#pragma once
#include "desc_base.hpp"
#include "scn_animation_clip_desc.h"
#include "scn_ecs_assembler.h"
#include "resources/res_resource_handle.hpp"

#include <vector>

namespace scn {

	struct animation_collection_component {
		std::vector<res::res_handle<animation_clip_desc>> clips;

		const animation_clip_desc* find(const std::string& anim_name) const
		{
			for (auto& handle : clips) {
				if (handle.is_ready()) {
					auto& clip = *handle.get_sync();
					if (clip.name == anim_name) {
						return &clip;
					}
				}
			}
			return nullptr;
		}
	};

	class animation_collection_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "animation_collection_desc";

		using base_type = desc::desc_base;

		animation_collection_desc() = default;
		~animation_collection_desc() = default;
		animation_collection_desc(const animation_collection_desc&) = default;
		animation_collection_desc(animation_collection_desc&&) = default;
		animation_collection_desc& operator=(const animation_collection_desc&) = default;
		animation_collection_desc& operator=(animation_collection_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			animation_collection_desc& other_desc = static_cast<animation_collection_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		std::vector<res::res_handle<animation_clip_desc>> clips;
	};

	void assemble_animation_collection(entt::registry& reg, entt::entity e,
	                                   const animation_collection_desc& desc, std::string_view name);

} // namespace scn
