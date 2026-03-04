#include "scn_prefab_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"

void scn::prefab_desc::deserialize(desc::desc_system& desc_system, const boost::json::object& data)
{
	auto parse_comp = [&] (const boost::json::object& obj, std::string_view explicit_type) -> prefab_comp_node {
		prefab_comp_node cnode;
		cnode.type_name = explicit_type;

		if (auto const* p = obj.if_contains("__parent")) {
			cnode.parent_desc = desc_system.get_field_desc<desc::desc_base>(*this, *p);
		}

		for (const auto& kv : obj) {
			if (kv.key() == "__type" || kv.key() == "__parent") continue;
			cnode.overrides[kv.key()] = kv.value();
		}
		return cnode;
	};

	auto parse_node = [&] (auto& self, std::string_view node_name, const boost::json::object& obj) -> prefab_node {
		prefab_node result;
		result.name = node_name;

		std::string_view type_name;
		if (auto const* p = obj.if_contains("__type")) {
			type_name = p->as_string();
		}

		if (type_name.empty()) {
			if (auto* transform = obj.if_contains("transform")) {
				auto& t_obj = transform->as_object();
				if (auto const* p = t_obj.if_contains("position")) result.position = json::value_to<glm::vec3>(*p);
				if (auto const* p = t_obj.if_contains("rotation")) result.rotation = json::value_to<glm::vec3>(*p);
				if (auto const* p = t_obj.if_contains("scale"))    result.scale = json::value_to<glm::vec3>(*p);
			}

			if (auto const* p = obj.if_contains("components")) {
				for (const auto& kv : p->as_object()) {
					std::string_view comp_key = kv.key();
					std::string_view comp_type = comp_key;

					if (auto const* t = kv.value().as_object().if_contains("__type")) {
						comp_type = t->as_string();
					}
					result.components[std::string(comp_key)] = parse_comp(kv.value().as_object(), comp_type);
				}
			}

			if (auto const* p = obj.if_contains("children")) {
				for (const auto& kv : p->as_object()) {
					std::string_view child_name = kv.key();
					result.children.push_back(self(self, child_name, kv.value().as_object()));
				}
			}
		} else {
			result.components[std::string(type_name)] = parse_comp(obj, type_name);
		}

		return result;
	};

	root = parse_node(parse_node, "", data);
}

void scn::prefab_desc::serialize(boost::json::object& data) const
{
	auto serialize_comp = [] (const prefab_comp_node& cnode, boost::json::object& out) {
		out["__type"] = cnode.type_name;
		if (cnode.parent_desc.is_valid()) {
			out["__parent"] = boost::json::value_from(cnode.parent_desc->get_tag());
		}
		for (const auto& kv : cnode.overrides) {
			out[kv.key()] = kv.value();
		}
	};

	auto serialize_node = [&] (auto& self, const prefab_node& nd, boost::json::object& node_obj) -> void {
		bool can_be_collapsed = nd.children.empty() &&
			nd.components.size() == 1 &&
			nd.position == glm::vec3{ 0.0f } &&
			nd.rotation == glm::vec3{ 0.0f } &&
			nd.scale == glm::vec3{ 1.0f } &&
			nd.components.begin()->first == nd.components.begin()->second.type_name;

		if (can_be_collapsed) {
			const auto& [comp_name, cnode] = *nd.components.begin();
			serialize_comp(cnode, node_obj);
			return;
		}

		boost::json::object transform_obj;
		if (nd.position != glm::vec3{ 0.0f }) transform_obj["position"] = boost::json::value_from(nd.position);
		if (nd.rotation != glm::vec3{ 0.0f }) transform_obj["rotation"] = boost::json::value_from(nd.rotation);
		if (nd.scale != glm::vec3{ 1.0f })    transform_obj["scale"] = boost::json::value_from(nd.scale);

		if (!transform_obj.empty()) {
			node_obj["transform"] = std::move(transform_obj);
		}

		if (!nd.components.empty()) {
			boost::json::object comps_obj;
			for (const auto& [comp_key, cnode] : nd.components) {
				boost::json::object c_obj;
				serialize_comp(cnode, c_obj);
				comps_obj[comp_key] = std::move(c_obj);
			}
			node_obj["components"] = std::move(comps_obj);
		}

		if (!nd.children.empty()) {
			boost::json::object children_arr;
			for (const auto& child_node : nd.children) {
				boost::json::object c_obj;
				self(self, child_node, c_obj);
				children_arr[child_node.name] = std::move(c_obj);
			}
			node_obj["children"] = std::move(children_arr);
		}
	};

	serialize_node(serialize_node, root, data);
}

void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node)
{
	for (const auto& [comp_name, cnode] : node.components) {
		assembler.assemble_and_apply(
			reg, e,
			cnode.type_name,
			cnode.parent_desc,
			cnode.overrides,
			comp_name
		);
	}

	for (const auto& child_node : node.children) {
		entt::entity child_entity = reg.create();

		if (child_node.position != glm::vec3{ 0.0f } || child_node.rotation != glm::vec3{ 0.0f } || child_node.scale != glm::vec3{ 1.0f }) {
			reg.emplace<scn::local_transform>(child_entity, child_node.get_local_transform());

			if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
				reg.ctx().get<ecs::event<scn::transform_updated>>().emit(child_entity);
			} else {
				ecs::event<scn::transform_updated> event;
				event.emit(child_entity);
				reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
			}
		}

		reg.emplace<scn::world_transform>(child_entity);
		reg.emplace<scn::name_component>(child_entity, scn::name_component{ .name = child_node.name });
		reg.emplace<scn::parent_component>(child_entity, e);
		reg.emplace<scn::depth_level>(child_entity);

		if (auto* children = reg.try_get<scn::children_component>(e)) {
			children->children.push_back(child_entity);
		} else {
			reg.emplace<scn::children_component>(e, std::vector{ child_entity });
		}

		if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
			reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(child_entity);
		} else {
			ecs::event<scn::hierarchy_updated> event;
			event.emit(child_entity);
			reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
		}

		assemble_prefab_node(assembler, reg, child_entity, child_node);
	}
}

void scn::assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name)
{
    const auto& root_node = prefab.get_root();
	if (root_node.position != glm::vec3{ 0.0f } || root_node.rotation != glm::vec3{ 0.0f } || root_node.scale != glm::vec3{ 1.0f }) {
		reg.emplace_or_replace<scn::local_transform>(e, root_node.get_local_transform());

		if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
			reg.ctx().get<ecs::event<scn::transform_updated>>().emit(e);
		} else {
			ecs::event<scn::transform_updated> event;
			event.emit(e);
			reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
		}
	}

	reg.emplace_or_replace<scn::world_transform>(e);
	if (!name.empty()) {
		reg.emplace_or_replace<scn::name_component>(e, scn::name_component{ .name = std::string(name) });
	}
	reg.emplace_or_replace<scn::depth_level>(e);
	if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
		reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(e);
	} else {
		ecs::event<scn::hierarchy_updated> event;
		event.emit(e);
		reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
	}

	assemble_prefab_node(assembler, reg, e, root_node);
}
