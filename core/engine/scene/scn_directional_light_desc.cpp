#include <snk/scene/scn_directional_light_desc.h>
#include <snk/scene/scn_glm_json_convert.h>

#include <snk/scene/scn_model.h>

namespace {
	// A malformed field keeps the current (default) value instead of collapsing to zero, which would black out the light.
	void read_vec4_field(const json::object& data, std::string_view key, glm::vec4& field)
	{
		const auto* v = data.if_contains(key);
		if (!v) return;
		if (!v->is_array() || v->as_array().size() != 4) {
			egLOG_WARN("scene/light", "directional_light_desc.{}: expected an array of 4 numbers, got '{}'; keeping default", key, json::serialize(*v));
			return;
		}
		for (const auto& n : v->as_array()) {
			if (!n.is_number()) {
				egLOG_WARN("scene/light", "directional_light_desc.{}: non-numeric component in '{}'; keeping default", key, json::serialize(*v));
				return;
			}
		}
		field = json::value_to<glm::vec4>(*v);
	}
}

void scn::directional_light_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	read_vec4_field(data, "direction", direction);
	read_vec4_field(data, "diffuse", diffuse);
	read_vec4_field(data, "ambient", ambient);
	read_vec4_field(data, "specular", specular);
}

void scn::directional_light_desc::serialize(json::object& data) const
{
	data["direction"] = json::value_from(direction);
	data["diffuse"] = json::value_from(diffuse);
	data["ambient"] = json::value_from(ambient);
	data["specular"] = json::value_from(specular);
}

void scn::assemble_directional_light(entt::registry& reg, entt::entity e, const directional_light_desc& desc, const std::string_view name)
{
	reg.emplace_or_replace<scn::renderable>(e);
	reg.emplace_or_replace<scn::directional_light>(e, scn::directional_light{ .direction = desc.direction, .diffuse = desc.diffuse, .ambient = desc.ambient, .specular = desc.specular });
}