#include "scn_animation_clip_desc.h"
#include "scn_glm_json_convert.h"

namespace scn
{
	template<class T>
	void tag_invoke(json::value_from_tag, json::value& out, const scn::animation_key_t<T>& c)
	{
		json::object object;
		object["value"] = json::value_from(c.value);
		object["time"] = c.time;
		out = std::move(object);
	}

	template<class T>
	scn::animation_key_t<T> tag_invoke(json::value_to_tag<scn::animation_key_t<T>>, const json::value& jv)
	{
		scn::animation_key_t<T> c;
		c.value = json::value_to<T>(jv.get_object().at("value"));
		c.time = static_cast<float>(jv.get_object().at("time").to_number<double>());
		return c;
	}

	void tag_invoke(json::value_from_tag, json::value& out, const scn::animation_node& c)
	{
		json::object object;
		object["position"] = json::value_from(c.pos_keys);
		object["rotation"] = json::value_from(c.rotate_keys);
		object["scale"] = json::value_from(c.scale_keys);
		out = std::move(object);
	}

	scn::animation_node tag_invoke(json::value_to_tag<scn::animation_node>, const json::value& jv)
	{
		scn::animation_node node;
		auto const& obj = jv.get_object();
		if (auto const* p = obj.if_contains("position"))
			node.pos_keys = json::value_to<std::vector<scn::animation_pos_key>>(*p);
		if (auto const* p = obj.if_contains("rotation"))
			node.rotate_keys = json::value_to<std::vector<scn::animation_rotate_key>>(*p);
		if (auto const* p = obj.if_contains("scale"))
			node.scale_keys = json::value_to<std::vector<scn::animation_scale_key>>(*p);
		return node;
	}
} // namespace scn

void scn::animation_clip_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	if (auto const* p = data.if_contains("name"))
		name = json::value_to<std::string>(*p);
	if (auto const* p = data.if_contains("duration"))
		duration = static_cast<float>(p->to_number<double>());
	if (auto const* p = data.if_contains("ticks_per_second"))
		ticks_per_second = static_cast<float>(p->to_number<double>());

	if (auto const* p = data.if_contains("channels")) {
		auto const& ch_obj = p->get_object();
		for (auto const& entry : ch_obj) {
			if (entry.value().is_object()) {
				scn::animation_node node = json::value_to<scn::animation_node>(entry.value());
				channels[std::string{ entry.key() }] = std::move(node);
			}
		}
	}
}

void scn::animation_clip_desc::serialize(json::object& data) const
{
	data["name"] = name;
	data["duration"] = duration;
	data["ticks_per_second"] = ticks_per_second;

	json::object ch_obj;
	for (auto const& [node_name, node] : channels) {
		ch_obj[node_name] = json::value_from(node);
	}
	data["channels"] = std::move(ch_obj);
}
