#include "scn_prefab_desc.h"
#include "logger/engine_log.h"
#include "scn_model.h"
#include "scn_node_index.h"
#include "scn_glm_json_convert.h"
#include "desc_node_id.h"
#include "res_system.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace scn {

	namespace {

		// Sets the component for a spawned child: the parent's path plus the child's own id.
		// Nodes without a valid id (hand-built descs) are simply not addressable.
		void apply_node_path(entt::registry& reg, entt::entity parent, entt::entity child, uint64_t node_id)
		{
			scn::node_index::get(reg); // hooks must exist before the first component is added

			const auto* parent_path = reg.try_get<scn::node_id_component>(parent);
			if (node_id == desc::invalid_node_id || !parent_path) {
				reg.remove<scn::node_id_component>(child);
				return;
			}
			scn::node_id_component comp{ parent_path->path };
			comp.path.push_back(node_id);
			reg.emplace_or_replace<scn::node_id_component>(child, std::move(comp));
		}

		uint64_t effective_node_id(const scn::prefab_desc::prefab_node* base, const scn::prefab_desc::prefab_node* over)
		{
			if (base && base->node_id != desc::invalid_node_id) return base->node_id;
			return over ? over->node_id : desc::invalid_node_id;
		}

		// Ids are unique per file. Explicit ids come first (a repeated one is dropped with a warning), then nodes
		// without one get the hash of their name path, which makes ids of old files stable across loads.
		void finalize_node_ids(scn::prefab_desc::prefab_node& root)
		{
			std::unordered_set<uint64_t> used;

			auto collect = [&] (auto& self, scn::prefab_desc::prefab_node& node, std::string& path) -> void {
				if (node.node_id != desc::invalid_node_id && !used.insert(node.node_id).second) {
					egLOG_WARN("desc", "duplicate node id {} at '{}', regenerating", desc::format_node_id(node.node_id), path);
					node.node_id = desc::invalid_node_id;
				}
				for (auto& child : node.children) {
					self(self, child, path);
				}
			};
			std::string path;
			collect(collect, root, path);

			auto assign = [&] (auto& self, scn::prefab_desc::prefab_node& node, const std::string& node_path) -> void {
				if (node.node_id == desc::invalid_node_id) {
					uint64_t id = desc::derive_node_id(node_path);
					while (used.contains(id)) {
						id = desc::new_node_id();
					}
					node.node_id = id;
					used.insert(id);
				}
				std::unordered_map<std::string_view, int> seen;
				for (auto& child : node.children) {
					const int index = seen[child.name]++;
					std::string child_path = node_path + "/" + child.name;
					if (index > 0) {
						child_path += "#" + std::to_string(index);
					}
					self(self, child, child_path);
				}
			};
			assign(assign, root, root.name);
		}
	}

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
			// __type/__parent are consumed above; other __* keys are file metadata, never overrides.
			if (kv.key().starts_with("__")) continue;
			cnode.overrides[kv.key()] = kv.value();
		}
		return cnode;
	};

	// A v2 node is { "__id", "name", "transform", "components", "children": [nodes] }; the root has no name.
	auto parse_node = [&] (auto& self, const json::object& obj, bool is_root) -> prefab_node {
		prefab_node result;
		if (auto const* p = obj.if_contains("name"); p && p->is_string() && !is_root) {
			result.name = p->as_string();
		}
		if (auto const* p = obj.if_contains("__id")) {
			auto id = p->is_string() ? desc::parse_node_id(p->as_string()) : std::nullopt;
			if (id) result.node_id = *id;
			else egLOG_WARN("desc", "node '{}' has a malformed __id, it will be derived", result.name);
		}
		if (auto* transform = obj.if_contains("transform"); transform && transform->is_object()) {
			auto& t_obj = transform->as_object();
			if (auto const* p = t_obj.if_contains("position")) result.position = json::value_to<glm::vec3>(*p);
			if (auto const* p = t_obj.if_contains("rotation")) result.rotation = json::value_to<glm::vec3>(*p);
			if (auto const* p = t_obj.if_contains("scale"))    result.scale = json::value_to<glm::vec3>(*p);
		}

		if (auto const* p = obj.if_contains("components"); p && p->is_object()) {
			for (const auto& kv : p->as_object()) {
				std::string_view comp_key = kv.key();
				result.components[std::string(comp_key)] = parse_comp(kv.value(), comp_key);
			}
		}

		if (auto const* p = obj.if_contains("children")) {
			if (!p->is_array()) {
				egLOG_ERROR("desc", "node '{}': 'children' must be an array (format v1 needs migration), ignoring it", result.name);
			} else {
				result.children.reserve(p->as_array().size());
				for (const auto& child : p->as_array()) {
					if (!child.is_object()) {
						egLOG_WARN("desc", "node '{}': child is not an object, skipping", result.name);
						continue;
					}
					result.children.push_back(self(self, child.as_object(), false));
				}
			}
		}
		return result;
	};

	prefab_node parsed = parse_node(parse_node, data, true);

	// With a __parent the adapter already copied the parent into this instance: layer on top of it.
	if (!data.contains("__parent")) {
		root = std::move(parsed);
		finalize_node_ids(root);
		return;
	}

	// Ids the overlay does not spell out are derived the same way the parent's were, so a same-named node lines up.
	const uint64_t explicit_root_id = parsed.node_id;
	finalize_node_ids(parsed);
	if (explicit_root_id != desc::invalid_node_id) root.node_id = explicit_root_id;

	if (auto const* transform = data.if_contains("transform"); transform && transform->is_object()) {
		const auto& t_obj = transform->as_object();
		if (t_obj.contains("position")) root.position = parsed.position;
		if (t_obj.contains("rotation")) root.rotation = parsed.rotation;
		if (t_obj.contains("scale"))    root.scale = parsed.scale;
	}
	for (auto& [key, comp] : parsed.components) {
		root.components[key] = std::move(comp);
	}
	for (auto& child : parsed.children) {
		auto it = std::ranges::find(root.children, child.node_id, &prefab_node::node_id);
		if (it != root.children.end()) *it = std::move(child);
		else root.children.push_back(std::move(child));
	}
	finalize_node_ids(root);
}

void scn::prefab_desc::serialize(json::object& data) const
{
	auto serialize_comp = [] (const prefab_comp_node& cnode) -> json::value {
		if (cnode.overrides.empty() && cnode.type_name.empty() && cnode.parent_desc.is_valid()) {
			return json::value_from(cnode.parent_desc.get_tag());
		}

		json::object out;
		if (!cnode.type_name.empty()) {
			out["__type"] = cnode.type_name;
		}
		if (cnode.parent_desc.is_valid()) {
			out["__parent"] = json::value_from(cnode.parent_desc.get_tag());
		}
		for (const auto& kv : cnode.overrides) {
			out[kv.key()] = kv.value();
		}
		return out;
	};

	auto serialize_node = [&] (auto& self, const prefab_node& nd, bool is_root) -> json::object {
		json::object node_obj;
		if (nd.node_id != desc::invalid_node_id)
			node_obj["__id"] = desc::format_node_id(nd.node_id);
		if (!is_root)
			node_obj["name"] = nd.name;

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
			// The components live in an unordered_map: sorted keys keep saved files stable.
			std::vector<const std::pair<const std::string, prefab_comp_node>*> sorted;
			sorted.reserve(nd.components.size());
			for (const auto& entry : nd.components) sorted.push_back(&entry);
			std::ranges::sort(sorted, {}, [] (const auto* entry) -> const std::string& { return entry->first; });

			json::object comps_obj;
			for (const auto* entry : sorted) {
				comps_obj[entry->first] = serialize_comp(entry->second);
			}
			node_obj["components"] = std::move(comps_obj);
		}

		if (!nd.children.empty()) {
			json::array children_arr;
			children_arr.reserve(nd.children.size());
			for (const auto& child_node : nd.children) {
				children_arr.push_back(self(self, child_node, false));
			}
			node_obj["children"] = std::move(children_arr);
		}

		return node_obj;
	};

	data = serialize_node(serialize_node, root, true);
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
				auto asset = assembler.get_resource_system().require_sync<scn::prefab_desc>(cnode.parent_desc.get_tag());
				if (asset.is_ready()) nested_base_root = &asset->get_root();

				reg.emplace_or_replace<scn::prefab_instance>(e, scn::prefab_instance{
					cnode.parent_desc.get_tag(),
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

		apply_node_path(reg, e, child_entity, effective_node_id(b_child, o_child));
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
	res::res_handle<scn::prefab_desc> nested_asset;

	if (override_node) {
		for (const auto& [comp_name, cnode] : override_node->components) {
			if (cnode.type_name == "prefab_desc" && cnode.parent_desc.is_valid()) {
				// cnode.parent_desc is the handle captured at first assembly and goes stale; re-resolve by tag.
				nested_asset = assembler.get_resource_system().require_sync<scn::prefab_desc>(cnode.parent_desc.get_tag());
				if (ENGINE_VERIFY(nested_asset.is_ready(), "nested prefab '{}' is not loaded, ignoring", cnode.parent_desc.get_tag().view())) {
					nested_base_root = &nested_asset->get_root();
				}

				reg.emplace_or_replace<scn::prefab_instance>(live_entity, scn::prefab_instance{
					cnode.parent_desc.get_tag(),
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

			apply_node_path(reg, live_entity, child_entity, effective_node_id(next_base, next_override));
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

			apply_node_path(reg, live_entity, new_child, effective_node_id(next_base, next_override));
			assemble_merged_node(assembler, reg, new_child, next_base, next_override);
			processed_entities.insert(new_child);
		}
	}

	// Re-fetch: spawning children above may have created the component or moved it.
	children_comp = reg.try_get<scn::children_component>(live_entity);
	if (children_comp) {
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

namespace {

	bool has_valid_node_id(const json::object& node)
	{
		auto it = node.find("__id");
		return it != node.end() && it->value().is_string() && desc::parse_node_id(it->value().as_string());
	}

	// A component object without __type took the component key as its type; spell that out.
	void spell_out_component_types(json::object& node)
	{
		auto comps = node.find("components");
		if (comps == node.end() || !comps->value().is_object()) return;
		for (auto& kv : comps->value().as_object()) {
			if (kv.value().is_object() && !kv.value().as_object().contains("__type")) {
				kv.value().as_object()["__type"] = std::string{ kv.key() };
			}
		}
	}

	json::array upgrade_children_v1(json::object& children, const std::string& parent_path);

	// Header keys first, then the rest in the original order.
	json::object upgrade_node_v1(std::string_view name, json::value& val, const std::string& path)
	{
		json::object src;
		if (val.is_object() && !val.as_object().contains("__type")) {
			src = std::move(val.as_object());
		} else {
			// v1 one-line form: the node is a single component named after the node.
			json::object comps;
			comps[name] = std::move(val);
			src["components"] = std::move(comps);
		}

		json::object out;
		out["__id"] = has_valid_node_id(src) ? src.at("__id") : json::value{ desc::format_node_id(desc::derive_node_id(path)) };
		out["name"] = std::string{ name };
		spell_out_component_types(src);
		for (auto& kv : src) {
			if (kv.key() == "__id" || kv.key() == "name" || kv.key() == "children") continue;
			out[kv.key()] = std::move(kv.value());
		}
		if (auto it = src.find("children"); it != src.end() && it->value().is_object()) {
			out["children"] = upgrade_children_v1(it->value().as_object(), path);
		}
		return out;
	}

	json::array upgrade_children_v1(json::object& children, const std::string& parent_path)
	{
		json::array out;
		out.reserve(children.size());
		for (auto& kv : children) {
			const std::string name{ kv.key() };
			out.push_back(upgrade_node_v1(name, kv.value(), parent_path + "/" + name));
		}
		return out;
	}
}

// v1 keyed children by name (the name was the identity). v2 stores an array of nodes with an explicit
// __id and name. Ids are the same name-path hash the loader derived for v1 files, so they stay stable.
void scn::migrate_prefab_v1(json::object& obj)
{
	if (obj.contains("__parent") && obj.contains("children")) {
		egLOG_WARN("desc", "a prefab with '__parent' lists children; v2 pairs them with the parent's nodes by '__id', "
			"v1 name matching is not migrated (ids are derived from the node names)");
	}

	if (!has_valid_node_id(obj)) {
		obj["__id"] = desc::format_node_id(desc::derive_node_id(""));
	}
	spell_out_component_types(obj);
	if (auto it = obj.find("children"); it != obj.end() && it->value().is_object()) {
		json::array upgraded = upgrade_children_v1(it->value().as_object(), "");
		obj["children"] = std::move(upgraded);
	}
}

void scn::regenerate_node_ids(scn::prefab_desc::prefab_node& node) {
	node.node_id = desc::new_node_id();
	for (auto& child : node.children) {
		regenerate_node_ids(child);
	}
}

void scn::trigger_hot_reload(scn::ecs_assembler& assembler, entt::registry& reg, res::res_handle<desc::desc_base> changed_prefab) {
	// Any desc (level, stat, component descs...) can be signalled here; only prefab-derived ones are prefabs.
	auto desc = std::dynamic_pointer_cast<const scn::prefab_desc>(changed_prefab.get());
	if (!desc) return;

	auto view = reg.view<scn::prefab_instance>();

	std::vector<entt::entity> roots_to_reload;
	for (auto entity : view) {
		const auto& instance = view.get<scn::prefab_instance>(entity);
		if (instance.source_prefab == changed_prefab->get_tag()) {
			roots_to_reload.push_back(entity);
		}
	}

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

	// The root of a spawn is the origin of its own paths; a node spawned by a parent already has its path.
	scn::node_index::get(reg);
	if (!reg.all_of<scn::node_id_component>(e) && !reg.all_of<scn::parent_component>(e))
		reg.emplace<scn::node_id_component>(e);

	// A node that references this prefab as a component already got its own transform from spawn_child;
	// the referenced root must not wipe it (it is only a fallback while the node is at identity).
	const auto* existing = reg.try_get<scn::local_transform>(e);
	if (!existing || existing->local == glm::mat4(1.0f))
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