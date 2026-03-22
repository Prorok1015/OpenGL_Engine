#include "scn_animation_collection_desc.h"
#include "scn_model.h"

void scn::animation_collection_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("clips")) {
		auto const& arr = p->get_array();
		clips.reserve(arr.size());
		for (auto const& item : arr) {
			clips.push_back(desc_system.get_field_desc<animation_clip_desc>(*this, item));
		}
	}
}

void scn::animation_collection_desc::serialize(json::object& data) const
{
	json::array arr;
	for (auto const& handle : clips) {
		if (handle.is_ready()) {
			json::object clip_obj;
			clip_obj["__type"] = "animation_clip_desc";
			handle.get_sync()->serialize(clip_obj);
			arr.push_back(std::move(clip_obj));
		}
	}
	data["clips"] = std::move(arr);
}

void scn::assemble_animation_collection(entt::registry& reg, entt::entity e,
                                        const animation_collection_desc& desc, std::string_view name)
{
	scn::animation_collection_component comp;
	comp.clips = desc.clips;
	reg.emplace_or_replace<scn::animation_collection_component>(e, std::move(comp));

	// Auto-create animation_controller if not already present and we have clips
	if (!reg.all_of<scn::animation_controller_component>(e) && !desc.clips.empty()) {
		auto& first_clip = *desc.clips[0].get_sync();
		scn::animation_controller_component ctrl;
		ctrl.current_animation = first_clip.name;
		ctrl.duration = first_clip.duration;
		ctrl.ticks_per_second = first_clip.ticks_per_second;
		ctrl.playing = true;
		ctrl.loop = true;
		reg.emplace<scn::animation_controller_component>(e, std::move(ctrl));
	}

}
