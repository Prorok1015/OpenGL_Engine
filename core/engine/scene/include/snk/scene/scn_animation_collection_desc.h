#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/resource/resources/res_resource_handle.hpp>

#include <vector>
#include <snk/common/snk_api.h>

namespace scn {

	struct animation_collection_component {
		std::vector<res::res_handle<animation_clip_desc>> clips;

		const animation_clip_desc* find(const std::string& anim_name) const
		{
			for (auto& handle : clips) {
				handle.wait(); // loaded in the background; a clip that is still pending could be the one asked for
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

	class SNK_API animation_collection_desc : public desc::desc_base
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
			copy_to_same(*this, other);
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		std::vector<res::res_handle<animation_clip_desc>> clips;
	};

	SNK_API void assemble_animation_collection(entt::registry& reg, entt::entity e,
	                                   const animation_collection_desc& desc, std::string_view name);

} // namespace scn
