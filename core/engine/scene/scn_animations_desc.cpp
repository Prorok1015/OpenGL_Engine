#include "scn_animations_desc.h"
#include "scn_model.h"

void scn::animations_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("animations")) {
		auto const& anim_obj = p->get_object();
		for (auto const& entry : anim_obj) {
			if (entry.value().is_object()) {
				scn::animation anim;
				anim.name = entry.key();
				auto const& anim_data = entry.value().get_object();
				if (auto const* dur = anim_data.if_contains("duration"))
					anim.duration = static_cast<float>(dur->to_number<double>());
				if (auto const* tps = anim_data.if_contains("ticks_per_second"))
					anim.ticks_per_second = static_cast<float>(tps->to_number<double>());
				animations.push_back(std::move(anim));
			}
		}
	}
}

void scn::animations_desc::serialize(json::object& data) const
{
	json::object anim_obj;
	for (auto const& anim : animations) {
		json::object entry;
		entry["duration"] = anim.duration;
		entry["ticks_per_second"] = anim.ticks_per_second;
		anim_obj[anim.name] = std::move(entry);
	}
	data["animations"] = std::move(anim_obj);
}

void scn::assemble_animations(entt::registry& reg, entt::entity e, const animations_desc& desc, std::string_view name)
{
	scn::animations_component comp;
	comp.animations = desc.animations;
	reg.emplace_or_replace<scn::animations_component>(e, std::move(comp));
}
