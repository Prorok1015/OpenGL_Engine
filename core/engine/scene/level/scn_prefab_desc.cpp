#include "scn_prefab_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"

void scn::prefab_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
    auto parse_node = [&] (auto& self, const std::string& default_name, const json::object& node_obj) -> prefab_node {
        prefab_node nd;

        nd.name = default_name;

        if (auto* transform = node_obj.if_contains("transform")) {
            auto& transform_obj = transform->as_object();
            if (auto const* p = transform_obj.if_contains("position")) {
                nd.position = json::value_to<glm::vec3>(*p);
            }

            if (auto const* p = transform_obj.if_contains("rotation")) {
                nd.rotation = json::value_to<glm::vec3>(*p);
            }

            if (auto const* p = transform_obj.if_contains("scale")) {
                nd.scale = json::value_to<glm::vec3>(*p);
            }
        }

        if (auto const* p = node_obj.if_contains("__type")) {
            nd.type_name = p->as_string();
        }

        if (auto const* p = node_obj.if_contains("__parent")) {
            nd.parent_desc = desc_system.get_field_desc<desc::desc_base>(*this, *p);
        }

        if (auto const* p = node_obj.if_contains("components")) {
            for (const auto& kv : p->as_object()) {
                std::string comp_key(kv.key());
                nd.components[comp_key] = self(self, comp_key, kv.value().as_object());
            }
        }

        if (auto const* p = node_obj.if_contains("children")) {
            for (const auto& child_val : p->as_object()) {
                nd.children.push_back(self(self, child_val.key(), child_val.value().as_object()));
            }
        }

        for (const auto& kv : node_obj) {
            std::string key(kv.key());
            if (key != "__type" && key != "__parent" 
                && key != "components" && key != "children") {
                nd.overrides[key] = kv.value();
            }
        }

        return nd;
    };

    root = parse_node(parse_node, "root", data);
}

void scn::prefab_desc::serialize(json::object& data) const
{
    auto serialize_node = [](auto& self, const prefab_node& nd, json::object& node_obj) -> void {
		
        for (const auto& kv : nd.overrides) {
            node_obj[kv.key()] = kv.value();
        }

        if (!nd.type_name.empty()) {
            node_obj["__type"] = nd.type_name;
        }

        if (!nd.parent_desc.is_valid()) {
            node_obj["__parent"] = json::value_from(nd.parent_desc->get_tag());
        }

		json::object transform_obj;
		if (nd.position != glm::vec3{ 0.0f }) {
            transform_obj["position"] = json::value_from(nd.position);
        }

		if (nd.rotation != glm::vec3{ 0.0f }) {
            transform_obj["rotation"] = json::value_from(nd.rotation);
        }

        if (nd.scale != glm::vec3{ 1.0f }) {
            transform_obj["scale"] = json::value_from(nd.scale);
		}

		if (!transform_obj.empty()) {
            node_obj["transform"] = transform_obj;
        }

        if (!nd.components.empty()) {
            json::object comps_obj;
            for (const auto& [comp_key, comp_node] : nd.components) {
				json::object comp_node_obj;
                self(self, comp_node, comp_node_obj);
                comps_obj[comp_key] = comp_node_obj;
            }
            node_obj["components"] = comps_obj;
        }

        if (!nd.children.empty()) {
            json::object children_arr;
            for (const auto& child_node : nd.children) {
                json::object comp_node_obj;
                self(self, child_node, comp_node_obj);
                children_arr[child_node.name] = comp_node_obj;
            }
            node_obj["children"] = children_arr;
        }

    };

    serialize_node(serialize_node, root, data);
}

void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node)
{
    for (const auto& [comp_name, comp_node] : node.components) {
        if (comp_node.type_name == "prefab_desc"){
			assemble_prefab_node(assembler, reg, e, comp_node);
        } else {
            assembler.assemble_and_apply(
                reg, e,
                comp_node.type_name,
                comp_node.parent_desc,
                comp_node.overrides,
                comp_name
            );
        }
    }

    for (const auto& child_node : node.children) {
        entt::entity child_entity = reg.create();

        if (child_node.position != glm::vec3{ 0.0f } || child_node.rotation != glm::vec3{ 0.0f } || child_node.scale != glm::vec3{ 1.0f }) {
            reg.emplace<scn::local_transform>(child_entity, child_node.get_transform());
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

        if (reg.all_of<scn::children_component>(e)) {
            auto& children = reg.get<scn::children_component>(e);
            children.children.push_back(child_entity);
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

        if (child_node.type_name == "prefab_desc") {
            assemble_prefab_node(assembler, reg, child_entity, child_node);
        } else {
            assembler.assemble_and_apply(
                reg, child_entity,
                child_node.type_name,
                child_node.parent_desc,
                child_node.overrides,
                child_node.name
            );
        }
    }
}

void scn::assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name)
{
    const auto& root_node = prefab.get_root();
	assemble_prefab_node(assembler, reg, e, root_node);
}
