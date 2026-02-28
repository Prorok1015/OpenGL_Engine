#include "scn_prefab_desc.h"

void scn::prefab_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
    auto parse_node = [&] (auto& self, const std::string& default_name, const json::object& node_obj) -> prefab_node {
        prefab_node nd;

        nd.name = default_name;
        if (auto const* p = node_obj.if_contains("name")) {
            nd.name = p->as_string();
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
            int child_idx = 0;
            for (const auto& child_val : p->as_array()) {
                std::string child_default_name = "child_" + std::to_string(child_idx++);
                nd.children.push_back(self(self, child_default_name, child_val.as_object()));
            }
        }

        for (const auto& kv : node_obj) {
            std::string key(kv.key());
            if (key != "name" && key != "__type" && key != "__parent" &&
                key != "components" && key != "children") {
                nd.overrides[key] = kv.value();
            }
        }

        return nd;
    };

    root = parse_node(parse_node, "root", data);
}

void scn::prefab_desc::serialize(json::object& data) const
{
    auto serialize_node = [](auto& self, const prefab_node& nd) -> json::object {
        json::object node_obj = nd.overrides;

        if (!nd.name.empty() && nd.name != "root" && !nd.name.starts_with("child_")) {
            node_obj["name"] = nd.name;
        }

        if (!nd.type_name.empty()) {
            node_obj["__type"] = nd.type_name;
        }

        if (!nd.parent_desc.has_error()) {
            node_obj["__parent"] = json::value_from(nd.parent_desc->get_tag());
        }

        if (!nd.components.empty()) {
            json::object comps_obj;
            for (const auto& [comp_key, comp_node] : nd.components) {
                comps_obj[comp_key] = self(self, comp_node);
            }
            node_obj["components"] = comps_obj;
        }

        if (!nd.children.empty()) {
            json::array children_arr;
            for (const auto& child_node : nd.children) {
                children_arr.push_back(self(self, child_node));
            }
            node_obj["children"] = children_arr;
        }

        return node_obj;
    };

    data = serialize_node(serialize_node, root);
}

void scn::assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string& name)
{
    const auto& root_node = prefab.get_root();

    for (const auto& [comp_name, comp_node] : root_node.components) {
        assembler.assemble_and_apply(
            reg, e,
            comp_node.type_name,
            comp_node.parent_desc,
            comp_node.overrides,
            comp_name
        );
    }

    for (const auto& child_node : root_node.children) {
        entt::entity child_entity = reg.create();

        // reg.emplace<ParentComponent>(child_entity, e); 

        assembler.assemble_and_apply(
            reg, child_entity,
            child_node.type_name,
            child_node.parent_desc,
            child_node.overrides,
            child_node.name
        );
    }
}
