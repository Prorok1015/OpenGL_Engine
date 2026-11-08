#pragma once
#include <entt/fwd.hpp>
#include <string_view>
#include <snk/common/snk_api.h>

namespace scn
{
	struct animation_controller_component;
	class animation_clip_desc;

	// Plays the clip named `clip` of the entity's animation_collection_component on its
	// animation_controller_component. `entity` is the one holding both (the model root), never one of its nodes.
	// - Another clip, or this clip once it stopped (a clip without loop that reached its end), starts from 0.
	// - This clip while it plays keeps its time and takes the new `loop` and `speed`, so a game may call it every frame.
	// - `speed` 0 holds the pose.
	// False, with a WARN "animation", and the controller unchanged: no controller or collection on `entity`, no clip
	// named `clip`, or a negative or non-finite `speed`.
	SNK_API bool play_animation(entt::registry& reg, entt::entity entity, std::string_view clip, bool loop = true,
		float speed = 1.0f);

	// The controller's clip with the duration and ticks per second it caches from it; time and state are untouched.
	// Shared by play_animation and the animation_controller_desc spawner.
	SNK_API void select_animation_clip(animation_controller_component& controller, const animation_clip_desc& clip);
}
