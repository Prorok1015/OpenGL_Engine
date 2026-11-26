#include <snk/render/rnd_material_desc.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/res_diagnostics.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <string_view>

void rnd::material_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	// The schema reads everything the hand-written reader read but one form, settled here on a copy of the object: a queue
	// that is neither a word nor null was no queue (the generic reader would take a number that names an enumerator).
	json::object rest = data;
	const auto* queue_value = rest.if_contains("queue");
	if (queue_value && !queue_value->is_string() && !queue_value->is_null()) {
		rest["queue"] = json::value();
	}

	// A program is shared by the materials that draw with it, so only a reference to a .shader.desc is taken. The load is
	// refused rather than the field emptied: a saved material without its shader would lose it for good.
	if (const auto* shader = rest.if_contains("shader"); shader && shader->is_object()) {
		const auto* type = shader->as_object().if_contains("__type");
		if (type && type->is_string()) {
			throw res::load_error(res::diagnostic_kind::reference, "/shader",
				"an inline shader_desc is not allowed in a material: refer to a .shader.desc file");
		}
	}

	// The topologies RHI R4 removed from the driver: files written before it may still name them.
	constexpr std::array<std::string_view, 6> removed_render_modes{ "TRIANGLE_FAN", "TRIANGLE_ADJ", "TRIANGLE_STRIP_ADJ", "LINE_LOOP", "LINE_ADJ", "LINE_STRIP_ADJ" };
	if (const auto* mode = rest.if_contains("render_mode"); mode && mode->is_string()) {
		const std::string_view word{ mode->as_string() };
		if (std::ranges::find(removed_render_modes, word) != removed_render_modes.end()) {
			egLOG_WARN("render/material", "render_mode \"{}\" does not exist any more, the material is drawn as TRIANGLE", word);
		}
	}

	desc::schema_desc<material_desc, desc::schema_kind::desc>::deserialize(desc_system, rest);

	// The schema's range is the editor's: a cutoff over 1 read from a file would discard every fragment (#1439).
	if (!(alpha_cutoff >= 0.0f && alpha_cutoff <= 1.0f)) {
		const float read = alpha_cutoff;
		alpha_cutoff = std::isnan(read) ? 1.0f : std::clamp(read, 0.0f, 1.0f);
		egLOG_WARN("render/material", "alpha_cutoff {} is outside [0, 1]: {} is used", read, alpha_cutoff);
	}
}
