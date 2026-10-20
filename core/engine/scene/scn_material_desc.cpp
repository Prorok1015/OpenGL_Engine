#include <snk/scene/scn_material_desc.h>
#include <snk/common/logger/engine_log.h>

#include <algorithm>
#include <array>
#include <string_view>

void scn::material_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	// The schema reads everything the hand-written reader read but one form, settled here on a copy of the object: a queue
	// that is neither a word nor null was no queue (the generic reader would take a number that names an enumerator).
	json::object rest = data;
	const auto* queue_value = rest.if_contains("queue");
	if (queue_value && !queue_value->is_string() && !queue_value->is_null()) {
		rest["queue"] = json::value();
	}

	// The topologies RHI R4 removed from the driver: files written before it may still name them.
	constexpr std::array<std::string_view, 6> removed_render_modes{ "TRIANGLE_FAN", "TRIANGLE_ADJ", "TRIANGLE_STRIP_ADJ", "LINE_LOOP", "LINE_ADJ", "LINE_STRIP_ADJ" };
	if (const auto* mode = rest.if_contains("render_mode"); mode && mode->is_string()) {
		const std::string_view word{ mode->as_string() };
		if (std::ranges::find(removed_render_modes, word) != removed_render_modes.end()) {
			egLOG_WARN("scene/material", "render_mode \"{}\" does not exist any more, the material is drawn as TRIANGLE", word);
		}
	}

	desc::schema_desc<material_desc, desc::schema_kind::desc>::deserialize(desc_system, rest);
}
