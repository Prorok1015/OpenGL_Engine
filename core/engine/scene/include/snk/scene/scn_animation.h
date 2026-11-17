#pragma once
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <string>
#include <string_view>
#include <snk/common/snk_api.h>

namespace scn
{
	struct animation_controller_component;
	class animation_clip_desc;

	// An event of the world bus (WORLD_BUS.md): a clip without loop reached its end and its controller stopped on the
	// last frame. scn::animation_controller_update emits it in phase update, so subscribers and the systems after the
	// delivery point get it the same frame. Not sent for a clip replaced or stopped before its end, nor when a loop goes
	// round. A clip that lands exactly on its end is not over yet: the end is past it (strictly), so the event comes the
	// next frame. Setting `playing` back to true on a stopped controller by hand plays the last frame again and sends the
	// event again. `entity` (the model root, as given to play_animation) may be destroyed by the time it is delivered, by
	// a system later in the same frame: check registry.valid(entity). Part of the module API: its layout stays as it is.
	struct animation_finished
	{
		entt::entity entity = entt::null;
		std::string clip;
	};

	// Its payload in the bus journal (#605): {"entity":<entt id as a number, null for entt::null>,"clip":"<name>"}.
	SNK_API std::string animation_finished_json(const animation_finished& event);

	// Plays the clip named `clip` of the entity's animation_collection_component on its
	// animation_controller_component. `entity` is the one holding both (the model root), never one of its nodes.
	// - Another clip, or this clip once it stopped (a clip without loop that reached its end), starts from 0.
	// - This clip while it plays keeps its time and takes the new `loop` and `speed` without looking the clip up again,
	//   so a game may call it every frame.
	// - `speed` 0 holds the pose.
	// It writes the controller: a system calling it needs write access to animation_controller_component (a non-const
	// view or the registry).
	// False, with a WARN "animation", and the controller unchanged: no controller or collection on `entity`, no clip
	// named `clip`, or a negative or non-finite `speed`.
	SNK_API bool play_animation(entt::registry& reg, entt::entity entity, std::string_view clip, bool loop = true,
		float speed = 1.0f);

	// The controller's clip with the duration and ticks per second it caches from it; time and state are untouched.
	// Shared by play_animation and the animation_controller_desc spawner.
	SNK_API void select_animation_clip(animation_controller_component& controller, const animation_clip_desc& clip);
}
