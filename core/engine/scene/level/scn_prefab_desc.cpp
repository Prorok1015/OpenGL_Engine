#include "scn_prefab_desc.h"
#include "scn_model.h"
#include "scn_glm_json_convert.h"
#include <unordered_set>

namespace scn {

	void assemble_merged_node(
		scn::ecs_assembler& assembler,
		entt::registry& reg,
		entt::entity e,
		const scn::prefab_desc::prefab_node* base_node,
		const scn::prefab_desc::prefab_node* override_node
	);

	void hot_reload_merged_node(
		scn::ecs_assembler& assembler,
		entt::registry& reg,
		entt::entity live_entity,
		const scn::prefab_desc::prefab_node* base_node,
		const scn::prefab_desc::prefab_node* override_node
	);

	void destroy_entity_hierarchy(entt::registry& reg, entt::entity entity) {
		if (auto* children_comp = reg.try_get<scn::children_component>(entity)) {
			auto children = children_comp->children;
			for (auto child : children) {
				destroy_entity_hierarchy(reg, child);
			}
		}
		reg.destroy(entity);
	}
}

void scn::prefab_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	auto parse_comp = [&] (const json::value& val, std::string_view explicit_type) -> prefab_comp_node {
		if (val.is_string()) {
			return prefab_comp_node{
				.parent_desc = desc_system.get_field_desc<desc::desc_base>(*this, val)
			};
		}

		if (!val.is_object())
			return {};

		const auto& obj = val.as_object();
		prefab_comp_node cnode;

		if (auto const* t = obj.if_contains("__type")) {
			cnode.type_name = t->as_string();
		} else {
			cnode.type_name = explicit_type;
		}

		if (auto const* p = obj.if_contains("__parent")) {
			cnode.parent_desc = desc_system.get_field_desc<desc::desc_base>(*this, *p);
		}

		for (const auto& kv : obj) {
			if (kv.key() == "__type" || kv.key() == "__parent") continue;
			cnode.overrides[kv.key()] = kv.value();
		}
		return cnode;
	};

	auto parse_node = [&] (auto& self, std::string_view node_name, const json::value& val, bool is_root = false) -> prefab_node {
		prefab_node result;
		result.name = node_name;

		if (is_root || (val.is_object() && !val.as_object().contains("__type"))) {
			const auto& obj = val.as_object();
			if (auto* transform = obj.if_contains("transform")) {
				auto& t_obj = transform->as_object();
				if (auto const* p = t_obj.if_contains("position")) result.position = json::value_to<glm::vec3>(*p);
				if (auto const* p = t_obj.if_contains("rotation")) result.rotation = json::value_to<glm::vec3>(*p);
				if (auto const* p = t_obj.if_contains("scale"))    result.scale = json::value_to<glm::vec3>(*p);
			}

			if (auto const* p = obj.if_contains("components")) {
				for (const auto& kv : p->as_object()) {
					std::string_view comp_key = kv.key();
					result.components[std::string(comp_key)] = parse_comp(kv.value(), comp_key);
				}
			}

			if (auto const* p = obj.if_contains("children")) {
				for (const auto& kv : p->as_object()) {
					result.children.push_back(self(self, kv.key(), kv.value()));
				}
			}
		} else {
			result.components[std::string(node_name)] = parse_comp(val, node_name);
		}

		return result;
	};

	root = parse_node(parse_node, "", data, true);
}

void scn::prefab_desc::serialize(json::object& data) const
{
	auto serialize_comp = [] (const std::string& comp_key, const prefab_comp_node& cnode, bool is_shorthand) -> json::value {
		if (cnode.overrides.empty() && cnode.type_name.empty() && cnode.parent_desc.is_valid()) {
			return json::value_from(cnode.parent_desc->get_tag());
		}

		json::object out;
		if (!cnode.type_name.empty() && (is_shorthand || cnode.type_name != comp_key)) {
			out["__type"] = cnode.type_name;
		}
		if (cnode.parent_desc.is_valid()) {
			out["__parent"] = json::value_from(cnode.parent_desc->get_tag());
		}
		for (const auto& kv : cnode.overrides) {
			out[kv.key()] = kv.value();
		}
		return out;
	};

	auto serialize_node = [&] (auto& self, const prefab_node& nd) -> json::value {
		bool can_be_collapsed = nd.children.empty() &&
			nd.components.size() == 1 &&
			nd.position == glm::vec3{ 0.0f } &&
			nd.rotation == glm::vec3{ 0.0f } &&
			nd.scale == glm::vec3{ 1.0f } &&
			nd.components.begin()->first == nd.name;

		if (can_be_collapsed) {
			const auto& [comp_name, cnode] = *nd.components.begin();
			return serialize_comp(comp_name, cnode, true);
		}

		json::object node_obj;

		json::object transform_obj;
		if (nd.position != glm::vec3{ 0.0f }) 
			transform_obj["position"] = json::value_from(nd.position);
		if (nd.rotation != glm::vec3{ 0.0f }) 
			transform_obj["rotation"] = json::value_from(nd.rotation);
		if (nd.scale != glm::vec3{ 1.0f })    
			transform_obj["scale"] = json::value_from(nd.scale);

		if (!transform_obj.empty()) 
			node_obj["transform"] = std::move(transform_obj);

		if (!nd.components.empty()) {
			json::object comps_obj;
			for (const auto& [comp_key, cnode] : nd.components) {
				comps_obj[comp_key] = serialize_comp(comp_key, cnode, false);
			}
			node_obj["components"] = std::move(comps_obj);
		}

		if (!nd.children.empty()) {
			json::object children_obj;
			for (const auto& child_node : nd.children) {
				children_obj[child_node.name] = self(self, child_node);
			}
			node_obj["children"] = std::move(children_obj);
		}

		return node_obj;
	};

	json::value root_val = serialize_node(serialize_node, root);
	if (root_val.is_object()) {
		data = root_val.as_object();
	}
}

void scn::assemble_merged_node(
	scn::ecs_assembler& assembler,
	entt::registry& reg,
	entt::entity e,
	const scn::prefab_desc::prefab_node* base_node,
	const scn::prefab_desc::prefab_node* override_node
) {
	const scn::prefab_desc::prefab_node* nested_base_root = nullptr;

	if (override_node) {
		for (const auto& [comp_name, cnode] : override_node->components) {
			if (cnode.type_name == "prefab_desc" && cnode.parent_desc.is_valid()) {
				auto asset = res::get_system().require_sync<scn::prefab_desc>(cnode.parent_desc->get_tag());
				if (asset.is_ready()) nested_base_root = &asset->get_root();

				reg.emplace_or_replace<scn::prefab_instance>(e, scn::prefab_instance{
					cnode.parent_desc->get_tag(),
					*override_node
				});
				break;
			}
		}
	}

	if (nested_base_root) base_node = nested_base_root;

	if (base_node) {
		for (const auto& [comp_name, cnode] : base_node->components) {
			if (cnode.type_name == "prefab_desc") continue;

			json::object final_overrides = cnode.overrides;
			if (override_node) {
				auto it = override_node->components.find(comp_name);
				if (it != override_node->components.end()) {
					for (const auto& kv : it->second.overrides) 
						final_overrides[kv.key()] = kv.value();
				}
			}
			assembler.assemble_and_apply(reg, e, cnode.type_name, cnode.parent_desc, final_overrides, comp_name);
		}
	}

	if (override_node) {
		for (const auto& [comp_name, cnode] : override_node->components) {
			if (cnode.type_name == "prefab_desc") continue;
			if (!base_node || base_node->components.find(comp_name) == base_node->components.end()) {
				assembler.assemble_and_apply(reg, e, cnode.type_name, cnode.parent_desc, cnode.overrides, comp_name);
			}
		}
	}

	std::unordered_map<std::string_view, const scn::prefab_desc::prefab_node*> override_children;
	if (override_node) {
		for (const auto& child : override_node->children) 
			override_children[child.name] = &child;
	}

	auto spawn_child = [&] (const scn::prefab_desc::prefab_node* b_child, const scn::prefab_desc::prefab_node* o_child) {
		entt::entity child_entity = reg.create();

		std::string final_name = (o_child && !o_child->name.empty()) ? 
			o_child->name : 
			(b_child ? b_child->name : "unnamed_child");
		glm::vec3 pos = (o_child && o_child->position != glm::vec3(0)) ? 
			o_child->position : 
			(b_child ? b_child->position : glm::vec3(0));
		glm::vec3 rot = (o_child && o_child->rotation != glm::vec3(0)) ? 
			o_child->rotation : 
			(b_child ? b_child->rotation : glm::vec3(0));
		glm::vec3 scl = (o_child && o_child->scale != glm::vec3(1)) ? 
			o_child->scale : 
			(b_child ? b_child->scale : glm::vec3(1));

		{
			scn::prefab_desc::prefab_node temp_node;
			temp_node.position = pos; temp_node.rotation = rot; temp_node.scale = scl;
			reg.emplace<scn::local_transform>(child_entity, temp_node.get_local_transform());
		}
		reg.emplace<scn::world_transform>(child_entity);

		if (reg.ctx().contains<ecs::event<scn::transform_updated>>())
			reg.ctx().get<ecs::event<scn::transform_updated>>().emit(child_entity);
		else {
			ecs::event<scn::transform_updated> event; event.emit(child_entity);
			reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
		}
		reg.emplace<scn::name_component>(child_entity, scn::name_component{ .name = final_name });
		reg.emplace<scn::parent_component>(child_entity, e);
		reg.emplace<scn::depth_level>(child_entity);

		if (auto* children = reg.try_get<scn::children_component>(e)) 
			children->children.push_back(child_entity);
		else reg.emplace<scn::children_component>(e, std::vector{ child_entity });

		if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(child_entity);
		else {
			ecs::event<scn::hierarchy_updated> event; event.emit(child_entity);
			reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
		}

		assemble_merged_node(assembler, reg, child_entity, b_child, o_child);
	};

	if (base_node) {
		for (const auto& base_child : base_node->children) {
			const scn::prefab_desc::prefab_node* over_child = nullptr;
			if (auto it = override_children.find(base_child.name); it != override_children.end()) {
				over_child = it->second;
				override_children.erase(it);
			}
			spawn_child(&base_child, over_child);
		}
	}

	for (const auto& [name, over_child] : override_children) {
		spawn_child(nullptr, over_child);
	}
}

void scn::hot_reload_merged_node(
	scn::ecs_assembler& assembler,
	entt::registry& reg,
	entt::entity live_entity,
	const scn::prefab_desc::prefab_node* base_node,
	const scn::prefab_desc::prefab_node* override_node
) {
	const scn::prefab_desc::prefab_node* nested_base_root = nullptr;

	if (override_node) {
		for (const auto& [comp_name, cnode] : override_node->components) {
			if (cnode.type_name == "prefab_desc" && cnode.parent_desc.is_valid()) {
				
				auto asset = ds::polymorphic_pointer_cast<const scn::prefab_desc>(cnode.parent_desc.get());
				 
				nested_base_root = &asset->get_root();

				reg.emplace_or_replace<scn::prefab_instance>(live_entity, scn::prefab_instance{
					cnode.parent_desc->get_tag(),
					*override_node
				});
				break;
			}
		}
	}

	if (nested_base_root) 
		base_node = nested_base_root;

	if (base_node) {
		for (const auto& [comp_name, cnode] : base_node->components) {
			if (cnode.type_name == "prefab_desc") continue;
			json::object final_overrides = cnode.overrides;
			if (override_node) {
				if (auto it = override_node->components.find(comp_name); it != override_node->components.end()) {
					for (const auto& kv : it->second.overrides) final_overrides[kv.key()] = kv.value();
				}
			}
			assembler.assemble_and_apply(reg, live_entity, cnode.type_name, cnode.parent_desc, final_overrides, comp_name);
		}
	}

	if (override_node) {
		for (const auto& [comp_name, cnode] : override_node->components) {
			if (cnode.type_name == "prefab_desc") continue;
			if (!base_node || base_node->components.find(comp_name) == base_node->components.end()) {
				assembler.assemble_and_apply(reg, live_entity, cnode.type_name, cnode.parent_desc, cnode.overrides, comp_name);
			}
		}
	}

	auto* children_comp = reg.try_get<scn::children_component>(live_entity);
	std::unordered_map<std::string_view, entt::entity> live_children_map;

	if (children_comp) {
		for (auto child : children_comp->children) {
			if (auto* name_comp = reg.try_get<scn::name_component>(child)) {
				live_children_map[name_comp->name] = child;
			}
		}
	}

	std::unordered_map<std::string_view, const scn::prefab_desc::prefab_node*> new_base_children;
	if (base_node) {
		for (const auto& child : base_node->children) 
			new_base_children[child.name] = &child;
	}

	std::unordered_map<std::string_view, const scn::prefab_desc::prefab_node*> new_override_children;
	if (override_node) {
		for (const auto& child : override_node->children) 
			new_override_children[child.name] = &child;
	}

	std::vector<std::string_view> all_new_names;
	for (auto const& [name, _] : new_base_children) 
		all_new_names.push_back(name);
	for (auto const& [name, _] : new_override_children) {
		if (new_base_children.find(name) == new_base_children.end()) 
			all_new_names.push_back(name);
	}

	std::unordered_set<entt::entity> processed_entities;

	for (const auto& child_name : all_new_names) {
		const auto* next_base = new_base_children.count(child_name) ? new_base_children[child_name] : nullptr;
		const auto* next_override = new_override_children.count(child_name) ? new_override_children[child_name] : nullptr;

		auto it = live_children_map.find(child_name);
		if (it != live_children_map.end()) {
			entt::entity child_entity = it->second;

			glm::vec3 pos = (next_override && next_override->position != glm::vec3(0)) ? 
				next_override->position : 
				(next_base ? next_base->position : glm::vec3(0));
			glm::vec3 rot = (next_override && next_override->rotation != glm::vec3(0)) ? 
				next_override->rotation : 
				(next_base ? next_base->rotation : glm::vec3(0));
			glm::vec3 scl = (next_override && next_override->scale != glm::vec3(1)) ? 
				next_override->scale : 
				(next_base ? next_base->scale : glm::vec3(1));

			{
				scn::prefab_desc::prefab_node temp_node;
				temp_node.position = pos;
				temp_node.rotation = rot;
				temp_node.scale = scl;
				reg.get_or_emplace<scn::local_transform>(child_entity).local = temp_node.get_local_transform();

				if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
					reg.ctx().get<ecs::event<scn::transform_updated>>().emit(child_entity);
				}
			}

			hot_reload_merged_node(assembler, reg, child_entity, next_base, next_override);
			processed_entities.insert(child_entity);
		} else {
			entt::entity new_child = reg.create();

			std::string final_name = (next_override && !next_override->name.empty()) ? 
				next_override->name : 
				(next_base ? next_base->name : "unnamed_child");
			glm::vec3 pos = (next_override && next_override->position != glm::vec3(0)) ? 
				next_override->position : 
				(next_base ? next_base->position : glm::vec3(0));
			glm::vec3 rot = (next_override && next_override->rotation != glm::vec3(0)) ? 
				next_override->rotation : 
				(next_base ? next_base->rotation : glm::vec3(0));
			glm::vec3 scl = (next_override && next_override->scale != glm::vec3(1)) ? 
				next_override->scale : 
				(next_base ? next_base->scale : glm::vec3(1));

			{
				scn::prefab_desc::prefab_node temp_node;
				temp_node.position = pos; temp_node.rotation = rot; temp_node.scale = scl;
				reg.emplace<scn::local_transform>(new_child, temp_node.get_local_transform());
			}
			reg.emplace<scn::world_transform>(new_child);

			if (reg.ctx().contains<ecs::event<scn::transform_updated>>())
				reg.ctx().get<ecs::event<scn::transform_updated>>().emit(new_child);
			else {
				ecs::event<scn::transform_updated> event; event.emit(new_child);
				reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
			}
			reg.emplace<scn::name_component>(new_child, scn::name_component{ .name = final_name });
			reg.emplace<scn::parent_component>(new_child, live_entity);
			reg.emplace<scn::depth_level>(new_child);

			if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) 
				reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(new_child);
			else {
				ecs::event<scn::hierarchy_updated> event; event.emit(new_child);
				reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
			}

			if (auto* children_c = reg.try_get<scn::children_component>(live_entity)) {
				children_c->children.push_back(new_child);
			} else {
				reg.emplace<scn::children_component>(live_entity, std::vector{ new_child });
			}

			assemble_merged_node(assembler, reg, new_child, next_base, next_override);
			processed_entities.insert(new_child);
		}
	}

	if (children_comp && !processed_entities.empty()) {
		auto& children_vec = children_comp->children;
		for (auto it = children_vec.begin(); it != children_vec.end(); ) {
			if (processed_entities.find(*it) == processed_entities.end()) {
				destroy_entity_hierarchy(reg, *it);
				it = children_vec.erase(it);
			} else {
				++it;
			}
		}
	}
}

void scn::trigger_hot_reload(scn::ecs_assembler& assembler, entt::registry& reg, res::res_handle<desc::desc_base> changed_prefab) {
	auto view = reg.view<scn::prefab_instance>();

	std::vector<entt::entity> roots_to_reload;
	for (auto entity : view) {
		const auto& instance = view.get<scn::prefab_instance>(entity);
		if (instance.source_prefab == changed_prefab->get_tag()) {
			roots_to_reload.push_back(entity);
		}
	}

	auto desc = ds::polymorphic_pointer_cast<const scn::prefab_desc>(changed_prefab.get());
	if (!desc) return;

	for (auto root_entity : roots_to_reload) {
		auto& instance = reg.get<scn::prefab_instance>(root_entity);
		hot_reload_merged_node(assembler, reg, root_entity, &desc->get_root(), &instance.local_overrides);
	}
}

void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node)
{
	scn::assemble_merged_node(assembler, reg, e, nullptr, &node);
}

void scn::assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name, res::tag source_prefab)
{
	const auto& root_node = prefab.get_root();

	reg.emplace_or_replace<scn::local_transform>(e, root_node.get_local_transform());
	reg.emplace_or_replace<scn::world_transform>(e);

	if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
		reg.ctx().get<ecs::event<scn::transform_updated>>().emit(e);
	} else {
		ecs::event<scn::transform_updated> event;
		event.emit(e);
		reg.ctx().emplace<ecs::event<scn::transform_updated>>(std::move(event));
	}
	if (!name.empty()) {
		reg.emplace_or_replace<scn::name_component>(e, scn::name_component{ .name = std::string(name) });
	}
	reg.emplace_or_replace<scn::depth_level>(e);

	if (!source_prefab.string().empty()) {
		reg.emplace_or_replace<scn::prefab_instance>(e, scn::prefab_instance{
			source_prefab,
			scn::prefab_desc::prefab_node{}
		});
	}

	if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
		reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(e);
	} else {
		ecs::event<scn::hierarchy_updated> event;
		event.emit(e);
		reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
	}

	scn::assemble_merged_node(assembler, reg, e, &root_node, nullptr);
}