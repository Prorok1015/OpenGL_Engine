#include "scn_animatable_prototype_desc.h"
#include "scn_glm_json_convert.h"
#include "scn_model.h"

namespace scn
{
	template<class T>
	void tag_invoke(json::value_from_tag, json::value& out, const scn::animatable_prototype_desc::keyframe_t<T>& c)
	{
		json::object object;
		object["value"] = json::value_from(c.value);
		object["time"] = json::value_from(c.time);
		out = object;
	}

	void tag_invoke(json::value_from_tag, json::value& out, const scn::animatable_prototype_desc::animatable_node_t& c)
	{
		json::object object;
		object["name"] = c.name;
		object["position_keyframe"] = json::value_from(c.pos_keyframes);
		object["rotation_keyframe"] = json::value_from(c.rotate_keyframes);
		object["scale_keyframe"] = json::value_from(c.scale_keyframes);
		out = object;

	}

	void tag_invoke(json::value_from_tag, json::value& out, const scn::animatable_prototype_desc::animation_t& c)
	{
		json::object object;
		object["name"] = json::value_from(c.name);
		object["duration"] = json::value_from(c.duration);
		object["ticks_per_second"] = json::value_from(c.ticks_per_second);
		object["keyframes"] = json::value_from(c.nodes);
		out = object;

	}

	template<class T>
	scn::animatable_prototype_desc::keyframe_t<T> tag_invoke(json::value_to_tag<scn::animatable_prototype_desc::keyframe_t<T>>, const json::value& data)
	{
		scn::animatable_prototype_desc::keyframe_t<T> c;
		c.value = json::value_to<T>(data.at("value"));
		c.time = json::value_to<float>(data.at("time"));
		return c;
	}

	scn::animatable_prototype_desc::animatable_node_t tag_invoke(json::value_to_tag<scn::animatable_prototype_desc::animatable_node_t>, const json::value& data)
	{
		using position_keyframe = scn::animatable_prototype_desc::position_keyframe;
		using rotation_keyframe = scn::animatable_prototype_desc::rotation_keyframe;
		using scale_keyframe = scn::animatable_prototype_desc::scale_keyframe;

		auto const& obj = data.get_object();
		scn::animatable_prototype_desc::animatable_node_t c;
		c.pos_keyframes = json::value_to<std::vector<position_keyframe>>(data.at("position"));
		c.rotate_keyframes = json::value_to<std::vector<rotation_keyframe>>(data.at("rotation"));
		c.scale_keyframes = json::value_to<std::vector<scale_keyframe>>(data.at("scale"));

		return c;
	}


	scn::animatable_prototype_desc::animation_t tag_invoke(json::value_to_tag<scn::animatable_prototype_desc::animation_t>, const json::value& data)
	{
		auto const& obj = data.get_object();
		scn::animatable_prototype_desc::animation_t animation;
		if (auto* duration = obj.if_contains("duration")) {
			animation.duration = json::value_to<float>(*duration);
		}

		if (auto* ticks_per_second = obj.if_contains("ticks_per_second")) {
			animation.ticks_per_second = json::value_to<float>(*ticks_per_second);
		}

		if (auto* nodes = obj.if_contains("keyframes")) {
			auto nodes_array = nodes->get_object();
			for (auto& node : nodes_array) {
				if (node.value().is_object()) {
					auto node_obj = json::value_to<animatable_prototype_desc::animatable_node_t>(node.value());
					node_obj.name = node.key();
					animation.nodes[node_obj.name] = node_obj;
				}
			}
		}

		return animation;
	}
} // namespace scn

void scn::animatable_prototype_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	prototype_desc::deserialize(desc_system, data);
	/*
		"animations": [
			"animation_name": {
				"duration": 1.0,
				"ticks_per_second": 30.0,
				"keyframes": [
					"node_name": {
						"position": [
							{ "value": [0, 0, 0], "time": 0.0 },
							{ "value": [1, 1, 1], "time": 1.0 }
						],
						"rotation": [
							{ "value": [0, 0, 0, 1], "time": 0.0 },
							{ "value": [1, 0, 0, 0], "time": 1.0 }
						],
						"scale": [
							{ "value": [1, 1, 1], "time": 0.0 },
							{ "value": [2, 2, 2], "time": 1.0 }
						]
					}
				]
			}
		]
	*/

	if (data.contains("animations")) {
		auto animations_array = data.at("animations").get_object();
		for (auto& anim : animations_array) {
			if (anim.value().is_object()) {
				auto animation = json::value_to<animation_t>(anim.value());
				animation.name = anim.key();
				animations[animation.name] = animation;
			}
		}
	}
}

void scn::animatable_prototype_desc::serialize(json::object& data) const
{
	prototype_desc::serialize(data);

	data["animations"] = json::value_from(animations);
}

void scn::animatable_prototype_desc::load_prototype_animations(entt::registry& registry, entt::entity root_ent) const
{
	scn::animations_component animations_comp;
	for (const auto& [_, anim] : animations) {
		scn::animation animation;
		animation.name = anim.name;
		animation.duration = anim.duration;
		animation.ticks_per_second = anim.ticks_per_second;
		animations_comp.animations.push_back(animation);
	}
	if (animations_comp.animations.size() > 0) {
		registry.emplace<scn::animations_component>(root_ent, animations_comp);
	}
}

entt::entity scn::animatable_prototype_desc::load_prototype(entt::registry& registry, entt::entity parent) const
{
	load_context ctx;
	auto root_ent = load_prototype_node(registry, parent, root, ctx);
	load_prototype_animations(registry, root_ent);
	return root_ent;
}

entt::entity scn::animatable_prototype_desc::load_prototype_node(entt::registry& registry, entt::entity parent, const node_t& node, load_context& ctx) const
{
	auto ent = prototype_desc::load_prototype_node(registry, parent, node, ctx);

	for (const auto& [_, animation] : animations)
	{
		if (auto it = animation.nodes.find(node.name); it != animation.nodes.end()) {
			auto& node_anim = it->second;
			auto& key = registry.get_or_emplace<scn::keyframes_component>(ent);
			scn::animation_node node_anim_res;
			for (auto& pos : node_anim.pos_keyframes) {
				node_anim_res.pos_keys.push_back({ pos.value, pos.time });
			}
			for (auto& rot : node_anim.rotate_keyframes) {
				node_anim_res.rotate_keys.push_back({ rot.value, rot.time });
			}
			for (auto& scale : node_anim.scale_keyframes) {
				node_anim_res.scale_keys.push_back({ scale.value, scale.time });
			}

			key.keyframes[animation.name] = node_anim_res;
		}
	}

	return ent;
}
