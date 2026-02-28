#include "scn_prefab_desc.h"
#include "scn_model.h"

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
    auto serialize_node = [](auto& self, const prefab_node& nd, json::object& node_obj) -> void {
		
        for (const auto& kv : nd.overrides) {
            node_obj[kv.key()] = kv.value();
        }

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
				json::object comp_node_obj;
                self(self, comp_node, comp_node_obj);
                comps_obj[comp_key] = comp_node_obj;
            }
            node_obj["components"] = comps_obj;
        }

        if (!nd.children.empty()) {
            json::array children_arr;
            for (const auto& child_node : nd.children) {
                json::object comp_node_obj;
                self(self, child_node, comp_node_obj);
                children_arr.push_back(comp_node_obj);
            }
            node_obj["children"] = children_arr;
        }

    };

    serialize_node(serialize_node, root, data);
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

        reg.emplace<scn::parent_component>(child_entity, e); 

        if (reg.all_of<scn::children_component>(e)) {
			auto& children = reg.get<scn::children_component>(e);
			children.children.push_back(child_entity);
        } else {
			reg.emplace<scn::children_component>(e, std::vector{ child_entity });
        }

        assembler.assemble_and_apply(
            reg, child_entity,
            child_node.type_name,
            child_node.parent_desc,
            child_node.overrides,
            child_node.name
        );
    }
}
