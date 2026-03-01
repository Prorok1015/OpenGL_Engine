#include "scn_anchor_desc.h"
#include "scn_glm_json_convert.h"
#include "scn_model.h"

void scn::scene_anchor_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (data.contains("position"))
		position = json::value_to<glm::vec3>(data.at("position"));
	if (data.contains("rotation"))
		rotation = json::value_to<glm::vec3>(data.at("rotation"));
	if (data.contains("scale"))
		scale = json::value_to<glm::vec3>(data.at("scale"));
}

void scn::scene_anchor_desc::serialize(json::object& data) const
{
	data["position"] = json::value_from(position);
	data["rotation"] = json::value_from(rotation);
	data["scale"] = json::value_from(scale);
}

void scn::assemble_scene_anchor(entt::registry& reg, entt::entity e, const scene_anchor_desc& desc, const std::string_view name)
{
	reg.emplace<scn::name_component>(e, std::string{ name });
	reg.emplace<scn::local_transform>(e, desc.get_transform());
	reg.emplace<scn::scene_anchor_component>(e);
	reg.emplace<scn::world_transform>(e);
	reg.emplace<scn::depth_level>(e);
	if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
		reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit((e));
	} else {
		ecs::event<scn::hierarchy_updated> event;
		event.emit((e));
		reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
	}
	if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
		reg.ctx().get<ecs::event<scn::transform_updated>>().emit((e));
	} else {
		ecs::event<scn::transform_updated> event;
		event.emit(e);
		reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
	}
}