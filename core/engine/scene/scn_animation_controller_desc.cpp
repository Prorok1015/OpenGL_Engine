#include "scn_animation_controller_desc.h"
#include "scn_model.h"
#include "scn_animation_collection_desc.h"

void scn::animation_controller_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("default_animation"))
		default_animation = p->as_string();
	if (auto const* p = data.if_contains("speed"))
		speed = static_cast<float>(p->to_number<double>());
	if (auto const* p = data.if_contains("autoplay"))
		autoplay = p->as_bool();
	if (auto const* p = data.if_contains("loop"))
		loop = p->as_bool();
}

void scn::animation_controller_desc::serialize(json::object& data) const
{
	if (!default_animation.empty())
		data["default_animation"] = default_animation;
	data["speed"] = speed;
	data["autoplay"] = autoplay;
	data["loop"] = loop;
}

void scn::assemble_animation_controller(entt::registry& reg, entt::entity e,
                                        const animation_controller_desc& desc, std::string_view name)
{
	scn::animation_controller_component ctrl;
	ctrl.speed = desc.speed;
	ctrl.loop = desc.loop;
	ctrl.playing = desc.autoplay;

	// If default_animation specified, use it; otherwise pick from animation_collection_component
	if (!desc.default_animation.empty()) {
		ctrl.current_animation = desc.default_animation;
	}

	if (auto* clips_comp = reg.try_get<scn::animation_collection_component>(e)) {
		// If no default specified, pick the first clip
		if (ctrl.current_animation.empty() && !clips_comp->clips.empty()) {
			if (clips_comp->clips[0].is_ready()) {
				ctrl.current_animation = clips_comp->clips[0].get_sync()->name;
			}
		}
		// Cache duration and ticks_per_second from the selected animation clip
		if (const auto* clip = clips_comp->find(ctrl.current_animation)) {
			ctrl.duration = clip->duration;
			ctrl.ticks_per_second = clip->ticks_per_second;
		}
	}

	reg.emplace_or_replace<scn::animation_controller_component>(e, std::move(ctrl));
}
