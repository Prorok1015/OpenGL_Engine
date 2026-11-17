#include <snk/scene/scn_animation.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <cmath>
#include <string>

std::string scn::animation_finished_json(const animation_finished& event)
{
	boost::json::object out;
	if (event.entity == entt::null) {
		out["entity"] = nullptr;
	} else {
		out["entity"] = entt::to_integral(event.entity);
	}
	out["clip"] = event.clip;
	return boost::json::serialize(out);
}

void scn::select_animation_clip(animation_controller_component& controller, const animation_clip_desc& clip)
{
	controller.current_animation = clip.name;
	controller.duration = clip.duration;
	controller.ticks_per_second = clip.ticks_per_second;
}

bool scn::play_animation(entt::registry& reg, entt::entity entity, std::string_view clip_name, bool loop, float speed)
{
	auto* controller = reg.valid(entity) ? reg.try_get<animation_controller_component>(entity) : nullptr;
	if (!controller) {
		egLOG_WARN("animation", "play_animation '{}': no animation controller on entity {}; pass the model root that holds it",
			clip_name, entt::to_integral(entity));
		return false;
	}
	if (!std::isfinite(speed) || speed < 0.0f) {
		egLOG_WARN("animation", "play_animation '{}' on entity {}: speed {} is not a finite value >= 0", clip_name,
			entt::to_integral(entity), speed);
		return false;
	}
	// The clip that plays goes on without a lookup: a call every frame never waits on a clip's resource handle.
	const bool continues = controller->playing && controller->current_animation == clip_name;
	if (!continues) {
		const auto* collection = reg.try_get<animation_collection_component>(entity);
		const animation_clip_desc* clip = collection ? collection->find(std::string{ clip_name }) : nullptr;
		if (!clip) {
			egLOG_WARN("animation", "play_animation: no clip '{}' in the animation collection of entity {}", clip_name,
				entt::to_integral(entity));
			return false;
		}
		select_animation_clip(*controller, *clip);
		controller->current_time = 0.0f;
	}
	controller->loop = loop;
	controller->speed = speed;
	controller->playing = true;
	return true;
}
