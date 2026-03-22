#pragma once
#include "desc_base.hpp"
#include "scn_mesh_nodes.hpp"

#include <unordered_map>
#include <string>

namespace scn {

	// A single animation clip containing keyframes for all animated nodes.
	// One clip per animation (e.g., "Idle", "Walk").
	// Can be stored as a separate .desc file or inlined in a prefab.
	class animation_clip_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "animation_clip_desc";

		using base_type = desc::desc_base;

		animation_clip_desc() = default;
		~animation_clip_desc() = default;
		animation_clip_desc(const animation_clip_desc&) = default;
		animation_clip_desc(animation_clip_desc&&) = default;
		animation_clip_desc& operator=(const animation_clip_desc&) = default;
		animation_clip_desc& operator=(animation_clip_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			animation_clip_desc& other_desc = static_cast<animation_clip_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		std::string name;
		float duration = 0.f;
		float ticks_per_second = 25.f;
		// node_name → keyframes for that node in this animation
		std::unordered_map<std::string, scn::animation_node> channels;
	};

} // namespace scn
