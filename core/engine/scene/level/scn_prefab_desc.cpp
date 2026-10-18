#include "scn_prefab_desc.h"
#include "logger/engine_log.h"
#include "scn_model.h"
#include "scn_node_index.h"
#include "scn_glm_json_convert.h"
#include "desc_node_id.h"
#include "desc_json_ops.h"
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

		using prefab_node = scn::prefab_desc::prefab_node;
		using prefab_comp_node = scn::prefab_desc::prefab_comp_node;

		// `owner` names the memory:// copies of inline descs, so it has to be the desc the JSON came from.
		prefab_comp_node parse_component(desc::desc_system& desc_system, const desc::desc_base& owner, const json::value& val, std::string_view explicit_type)
		{
			if (val.is_string()) {
				return prefab_comp_node{
					.parent_desc = desc_system.get_field_desc<desc::desc_base>(owner, val)
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
				cnode.parent_desc = desc_system.get_field_desc<desc::desc_base>(owner, *p);
			}

			for (const auto& kv : obj) {
				// __type/__parent are consumed above; other __* keys are file metadata, never overrides.
				if (kv.key().starts_with("__")) continue;
				cnode.overrides[kv.key()] = kv.value();
			}
			return cnode;
		}

		// A node is { "__id", "name", "transform", "components", "overrides": [records], "children": [nodes] };
		// the root has no name.
		prefab_node parse_node(desc::desc_system& desc_system, const desc::desc_base& owner, const json::object& obj, bool is_root)
		{
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
				result.has_transform = true;
				if (auto const* p = t_obj.if_contains("position")) result.position = json::value_to<glm::vec3>(*p);
				if (auto const* p = t_obj.if_contains("rotation")) result.rotation = json::value_to<glm::vec3>(*p);
				if (auto const* p = t_obj.if_contains("scale"))    result.scale = json::value_to<glm::vec3>(*p);
			}

			if (auto const* p = obj.if_contains("components"); p && p->is_object()) {
				for (const auto& kv : p->as_object()) {
					std::string_view comp_key = kv.key();
					result.components[std::string(comp_key)] = parse_component(desc_system, owner, kv.value(), comp_key);
				}
			}

			if (auto const* p = obj.if_contains("overrides")) {
				if (!p->is_array()) {
					egLOG_ERROR("desc", "node '{}': 'overrides' must be an array of records, ignoring it", result.name);
				} else {
					result.overrides.reserve(p->as_array().size());
					for (const auto& element : p->as_array()) {
						if (auto record = scn::parse_override_record(element)) {
							result.overrides.push_back(std::move(*record));
						}
					}
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
						result.children.push_back(parse_node(desc_system, owner, child.as_object(), false));
					}
				}
			}
			return result;
		}
	}
}

void scn::prefab_desc::deserialize(desc::desc_system& desc_system, const json::object& data)
{
	prefab_node parsed = parse_node(desc_system, *this, data, true);

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
		root.has_transform = true;
		if (t_obj.contains("position")) root.position = parsed.position;
		if (t_obj.contains("rotation")) root.rotation = parsed.rotation;
		if (t_obj.contains("scale"))    root.scale = parsed.scale;
	}
	for (auto& [key, comp] : parsed.components) {
		root.components[key] = std::move(comp);
	}
	// Records of the file come after the parent's own: they are applied to the merged tree when it is built.
	root.overrides.insert(root.overrides.end(), std::make_move_iterator(parsed.overrides.begin()), std::make_move_iterator(parsed.overrides.end()));
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

	// Not a generic `auto& self` lambda: instantiating that one crashes Clang 18 (frontend segfault).
	std::function<json::object(const prefab_node&, bool)> serialize_node = [&] (const prefab_node& nd, bool is_root) -> json::object {
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

		// An empty "transform" is kept for a node that instantiates a prefab: it says "placed at the origin"
		// instead of "use the prefab root's transform".
		const bool references_desc = std::ranges::any_of(nd.components, [] (const auto& entry) { return entry.second.parent_desc.is_valid(); });
		if (!transform_obj.empty() || (nd.has_transform && references_desc))
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

		if (!nd.overrides.empty()) {
			auto records = nd.overrides;
			scn::canonicalize_overrides(records);
			json::array records_arr;
			records_arr.reserve(records.size());
			for (const auto& record : records) {
				records_arr.push_back(scn::override_record_to_json(record));
			}
			node_obj["overrides"] = std::move(records_arr);
		}

		if (!nd.children.empty()) {
			json::array children_arr;
			children_arr.reserve(nd.children.size());
			for (const auto& child_node : nd.children) {
				children_arr.push_back(serialize_node(child_node, false));
			}
			node_obj["children"] = std::move(children_arr);
		}

		return node_obj;
	};

	data = serialize_node(root, true);
}

namespace {

	using prefab_node = scn::prefab_desc::prefab_node;
	using prefab_comp_node = scn::prefab_desc::prefab_comp_node;

	template<class Event>
	void emit_event(entt::registry& reg, entt::entity e)
	{
		if (reg.ctx().contains<ecs::event<Event>>()) {
			reg.ctx().get<ecs::event<Event>>().emit(e);
		} else {
			ecs::event<Event> event;
			event.emit(e);
			reg.ctx().emplace<ecs::event<Event>>(std::move(event));
		}
	}

	entt::entity create_child_entity(entt::registry& reg, entt::entity parent, std::string name, const glm::mat4& local)
	{
		entt::entity child = reg.create();
		reg.emplace<scn::local_transform>(child, local);
		reg.emplace<scn::world_transform>(child);
		emit_event<scn::transform_updated>(reg, child);
		reg.emplace<scn::name_component>(child, scn::name_component{ .name = std::move(name) });
		reg.emplace<scn::parent_component>(child, parent);
		reg.emplace<scn::depth_level>(child);

		if (auto* children = reg.try_get<scn::children_component>(parent))
			children->children.push_back(child);
		else reg.emplace<scn::children_component>(parent, std::vector{ child });

		emit_event<scn::hierarchy_updated>(reg, child);
		return child;
	}

	constexpr int max_instance_depth = 16;

	struct prefab_ref {
		std::string key;
		std::shared_ptr<const scn::prefab_desc> prefab;
	};

	// The component of `node` that makes it an instance of a prefab: a typed { "__parent": X } component or a
	// plain string reference that resolves to a prefab. Looked up by tag every time, a handle kept in the node
	// goes stale when the prefab is reloaded.
	std::optional<prefab_ref> find_prefab_ref(scn::ecs_assembler& assembler, const prefab_node& node)
	{
		std::vector<const std::pair<const std::string, prefab_comp_node>*> candidates;
		for (const auto& entry : node.components) {
			const auto& cnode = entry.second;
			const bool may_be_prefab = cnode.type_name.empty() || cnode.type_name == "prefab_desc" || cnode.type_name == "world_desc";
			if (may_be_prefab && cnode.parent_desc.is_valid() && !cnode.parent_desc.has_error()) {
				candidates.push_back(&entry);
			}
		}
		std::ranges::sort(candidates, {}, [] (const auto* entry) -> const std::string& { return entry->first; });

		for (const auto* entry : candidates) {
			auto loaded = assembler.get_resource_system().require_sync<desc::desc_base>(entry->second.parent_desc.get_tag());
			if (!loaded.is_ready()) {
				if (entry->second.type_name == "prefab_desc") {
					egLOG_ERROR("desc", "nested prefab '{}' is not loaded, ignoring", entry->second.parent_desc.get_tag().view());
				}
				continue;
			}
			if (auto prefab = std::dynamic_pointer_cast<const scn::prefab_desc>(loaded.get())) {
				return prefab_ref{ entry->first, std::move(prefab) };
			}
		}
		return std::nullopt;
	}

	void add_or_merge_node(desc::desc_system& desc_system, const desc::desc_base& owner, std::vector<prefab_node>& siblings, const json::object& value);

	// Overlays `src` (the JSON of a node) onto an existing node: only what the JSON spells out changes.
	void merge_into_node(desc::desc_system& desc_system, const desc::desc_base& owner, prefab_node& dst, const json::object& src)
	{
		prefab_node parsed = scn::parse_node(desc_system, owner, src, false);
		if (src.contains("name")) dst.name = parsed.name;
		if (auto* transform = src.if_contains("transform"); transform && transform->is_object()) {
			const auto& t_obj = transform->as_object();
			dst.has_transform = true;
			if (t_obj.contains("position")) dst.position = parsed.position;
			if (t_obj.contains("rotation")) dst.rotation = parsed.rotation;
			if (t_obj.contains("scale"))    dst.scale = parsed.scale;
		}
		for (auto& [key, comp] : parsed.components) {
			auto it = dst.components.find(key);
			if (it == dst.components.end()) {
				dst.components.emplace(key, std::move(comp));
			} else if (!comp.overrides.empty()) {
				it->second.edits.push_back({ "", comp.overrides });
			}
		}
		dst.overrides.insert(dst.overrides.end(), std::make_move_iterator(parsed.overrides.begin()), std::make_move_iterator(parsed.overrides.end()));
		if (auto* children = src.if_contains("children"); children && children->is_array()) {
			for (const auto& child : children->as_array()) {
				if (child.is_object()) add_or_merge_node(desc_system, owner, dst.children, child.as_object());
			}
		}
	}

	void add_or_merge_node(desc::desc_system& desc_system, const desc::desc_base& owner, std::vector<prefab_node>& siblings, const json::object& value)
	{
		std::optional<uint64_t> id;
		if (const auto* raw = value.if_contains("__id"); raw && raw->is_string()) {
			id = desc::parse_node_id(raw->as_string());
		}
		if (id) {
			if (auto existing = std::ranges::find(siblings, *id, &prefab_node::node_id); existing != siblings.end()) {
				merge_into_node(desc_system, owner, *existing, value);
				return;
			}
		}
		prefab_node added = scn::parse_node(desc_system, owner, value, false);
		if (!id) {
			egLOG_WARN("desc", "add_node: node '{}' has no valid __id, it gets a new one", added.name);
			added.node_id = desc::new_node_id();
		}
		siblings.push_back(std::move(added));
	}

	void apply_transform_record(prefab_node& node, const scn::override_record& record)
	{
		json::value transform = json::object{
			{ "position", json::value_from(node.position) },
			{ "rotation", json::value_from(node.rotation) },
			{ "scale", json::value_from(node.scale) },
		};
		if (record.path.empty()) {
			if (!record.value.is_object()) {
				egLOG_WARN("desc", "override: a transform record at '' needs an object value, skipping");
				return;
			}
			desc::json_deep_merge(transform.as_object(), record.value.as_object());
		} else if (!desc::json_pointer_set(transform, record.path, record.value)) {
			egLOG_WARN("desc", "override: bad transform pointer '{}', skipping", record.path);
			return;
		}

		auto is_vec3 = [] (const json::value* v) {
			if (!v || !v->is_array() || v->as_array().size() != 3) return false;
			return std::ranges::all_of(v->as_array(), [] (const json::value& n) { return n.is_number(); });
		};
		const auto& t = transform.as_object();
		if (!is_vec3(t.if_contains("position")) || !is_vec3(t.if_contains("rotation")) || !is_vec3(t.if_contains("scale"))) {
			egLOG_WARN("desc", "override: transform record at '{}' does not leave three numbers per field, skipping", record.path);
			return;
		}
		node.position = json::value_to<glm::vec3>(t.at("position"));
		node.rotation = json::value_to<glm::vec3>(t.at("rotation"));
		node.scale = json::value_to<glm::vec3>(t.at("scale"));
		node.has_transform = true;
	}

	void apply_set_record(desc::desc_system& desc_system, const desc::desc_base& owner, prefab_node& node, const scn::override_record& record)
	{
		if (record.component == scn::transform_pseudo_component) {
			apply_transform_record(node, record);
			return;
		}

		auto it = node.components.find(record.component);
		if (it == node.components.end()) {
			if (record.path.empty() && (record.value.is_object() || record.value.is_string())) {
				node.components.emplace(record.component, scn::parse_component(desc_system, owner, record.value, record.component));
			} else {
				egLOG_WARN("desc", "override: component '{}' not found, cannot set '{}'", record.component, record.path);
			}
			return;
		}
		if (record.path.empty() && record.value.is_string()) {
			// a plain reference to a desc replaces the component whole
			it->second = scn::parse_component(desc_system, owner, record.value, record.component);
			return;
		}
		if (record.path.empty() && !record.value.is_object()) {
			egLOG_WARN("desc", "override: component '{}' can only be merged with an object at ''", record.component);
			return;
		}
		it->second.edits.push_back({ record.path, record.value });
	}

	// Applies the records to a tree that is already a private copy. A record that targets a node below an
	// instance of another prefab is handed down to that instance, which resolves it once it is expanded.
	void apply_records(scn::ecs_assembler& assembler, const desc::desc_base& owner, prefab_node& root, const std::vector<scn::override_record>& records)
	{
		auto& desc_system = assembler.get_desc_system();
		for (const auto& record : records) {
			prefab_node* parent = nullptr;
			prefab_node* node = &root;
			bool resolved = true;
			for (size_t depth = 0; depth < record.target.size(); ++depth) {
				auto child = std::ranges::find(node->children, record.target[depth], &prefab_node::node_id);
				if (child == node->children.end()) {
					resolved = false;
					if (find_prefab_ref(assembler, *node)) {
						scn::override_record forwarded = record;
						forwarded.target.erase(forwarded.target.begin(), forwarded.target.begin() + depth);
						node->overrides.push_back(std::move(forwarded));
					} else {
						egLOG_WARN("desc", "override: no node {} below '{}', record skipped", desc::format_node_id(record.target[depth]), node->name);
					}
					break;
				}
				parent = node;
				node = &*child;
			}
			if (!resolved) continue;

			switch (record.op) {
			case scn::override_op::add_node:
				add_or_merge_node(desc_system, owner, node->children, record.value.as_object());
				break;

			case scn::override_op::remove:
				if (!record.component.empty()) {
					if (!record.path.empty()) {
						egLOG_WARN("desc", "override: 'remove' of '{}' with a path is not supported, skipped", record.component);
					} else if (record.component == scn::transform_pseudo_component || node->components.erase(record.component) == 0) {
						egLOG_WARN("desc", "override: no component '{}' to remove on '{}'", record.component, node->name);
					}
				} else if (!parent) {
					egLOG_WARN("desc", "override: the root of an instance cannot be removed, record skipped");
				} else {
					std::erase_if(parent->children, [node] (const prefab_node& candidate) { return &candidate == node; });
				}
				break;

			case scn::override_op::set:
				apply_set_record(desc_system, owner, *node, record);
				break;
			}
		}
	}

	// What `node` stands for when it is more than its own fields: the prefab it references (itself expanded)
	// plus the node's own extra components and children, then the node's override records. False for a plain
	// node without records. `source` is the referenced prefab, empty when there is none.
	bool materialize(scn::ecs_assembler& assembler, const prefab_node& node, prefab_node& out, res::tag& source, int depth)
	{
		std::optional<prefab_ref> ref = find_prefab_ref(assembler, node);
		if (ref && depth >= max_instance_depth) {
			egLOG_ERROR("desc", "prefab '{}' instantiates itself (nesting deeper than {}), reference ignored", ref->prefab->get_tag().view(), max_instance_depth);
			ref.reset();
		}
		if (!ref && node.overrides.empty()) return false;

		scn::prefab_desc fallback_owner;
		const desc::desc_base* owner = &fallback_owner;
		if (ref) {
			owner = ref->prefab.get();
			source = ref->prefab->get_tag();
			if (!materialize(assembler, ref->prefab->get_root(), out, source, depth + 1)) {
				out = ref->prefab->get_root();
			}
			source = ref->prefab->get_tag();
			for (const auto& [key, comp] : node.components) {
				if (key != ref->key) out.components[key] = comp;
			}
			out.children.insert(out.children.end(), node.children.begin(), node.children.end());
		} else {
			out = node;
		}
		out.overrides.clear();
		apply_records(assembler, *owner, out, node.overrides);
		return true;
	}

	// serialize(parent) with the component's own overrides merged in, then the edits in order.
	json::object compose_component_json(const prefab_comp_node& cnode, const desc::desc_base* parent)
	{
		json::object effective;
		if (parent) parent->serialize(effective);
		desc::json_deep_merge(effective, cnode.overrides);

		json::value holder = std::move(effective);
		for (const auto& edit : cnode.edits) {
			if (edit.path.empty()) {
				json::object mergeable;
				for (const auto& member : edit.value.as_object()) {
					if (!member.key().starts_with("__")) mergeable.insert_or_assign(member.key(), member.value());
				}
				desc::json_deep_merge(holder.as_object(), mergeable);
			} else if (!desc::json_pointer_set(holder, edit.path, edit.value)) {
				egLOG_WARN("desc", "override: cannot set '{}', skipped", edit.path);
			}
		}
		return std::move(holder.as_object());
	}

	// A node that instantiates a prefab already got its transform from the node itself; the prefab's root
	// transform is only the fallback while that is still the identity.
	void apply_root_transform_fallback(entt::registry& reg, entt::entity e, const prefab_node& prefab_root)
	{
		const auto* existing = reg.try_get<scn::local_transform>(e);
		if (!existing || existing->local == glm::mat4(1.0f)) {
			reg.emplace_or_replace<scn::local_transform>(e, prefab_root.get_local_transform());
		}
	}

	void destroy_entity_hierarchy(entt::registry& reg, entt::entity entity)
	{
		if (auto* children_comp = reg.try_get<scn::children_component>(entity)) {
			auto children = children_comp->children;
			for (auto child : children) {
				destroy_entity_hierarchy(reg, child);
			}
		}
		reg.destroy(entity);
	}

	// Live children of a node, claimed by node id (their node_id_component) or, for nodes that carry no id, by name.
	class live_children {
	public:
		live_children() = default;

		live_children(entt::registry& reg, entt::entity parent)
		{
			auto* children_comp = reg.try_get<scn::children_component>(parent);
			if (!children_comp) return;
			for (auto child : children_comp->children) {
				entry e{ child, desc::invalid_node_id, {} };
				if (const auto* path = reg.try_get<scn::node_id_component>(child); path && !path->path.empty())
					e.id = path->path.back();
				if (const auto* name = reg.try_get<scn::name_component>(child))
					e.name = name->name;
				m_entries.push_back(std::move(e));
			}
			for (size_t i = 0; i < m_entries.size(); ++i) {
				if (m_entries[i].id != desc::invalid_node_id) m_by_id.emplace(m_entries[i].id, i);
			}
		}

		entt::entity take(uint64_t node_id, std::string_view name)
		{
			if (node_id != desc::invalid_node_id) {
				if (auto it = m_by_id.find(node_id); it != m_by_id.end() && !m_entries[it->second].taken) {
					return claim(it->second);
				}
			}
			for (size_t i = 0; i < m_entries.size(); ++i) {
				auto& e = m_entries[i];
				// A node with an id only falls back to live nodes that have none.
				if (e.taken || e.name != name || (node_id != desc::invalid_node_id && e.id != desc::invalid_node_id)) continue;
				return claim(i);
			}
			return entt::null;
		}

	private:
		struct entry {
			entt::entity entity;
			uint64_t id;
			std::string name;
			bool taken = false;
		};

		entt::entity claim(size_t index)
		{
			m_entries[index].taken = true;
			return m_entries[index].entity;
		}

		std::vector<entry> m_entries;
		std::unordered_map<uint64_t, size_t> m_by_id;
	};
}

void scn::reconcile(
	scn::ecs_assembler& assembler,
	entt::registry& reg,
	entt::entity live,
	const scn::prefab_desc::prefab_node& node,
	reconcile_mode mode
) {
	// `node` itself unless it references a prefab or carries override records; `node` may be a part of this
	// entity's prefab_instance, which is replaced below, so nothing reads it after that.
	prefab_node expanded;
	const prefab_node* content = &node;
	res::tag source;
	if (materialize(assembler, node, expanded, source, 0)) {
		if (!source.string().empty()) {
			// A transform the node spells out places the instance; otherwise the prefab root's does, with the
			// target-[] transform records applied. Taken before the prefab_instance below replaces `node`.
			const glm::mat4 placement = (node.has_transform ? node : expanded).get_local_transform();
			if (!node.has_transform || !reg.all_of<scn::local_transform>(live)) {
				reg.emplace_or_replace<scn::local_transform>(live, placement);
			}
			reg.emplace_or_replace<scn::prefab_instance>(live, scn::prefab_instance{ source, node });
			emit_event<scn::transform_updated>(reg, live);
		}
		content = &expanded;
	}

	for (const auto& [comp_name, cnode] : content->components) {
		if (cnode.type_name == "prefab_desc") continue;

		if (cnode.edits.empty()) {
			assembler.assemble_and_apply(reg, live, cnode.type_name, cnode.parent_desc, cnode.overrides, comp_name);
			continue;
		}

		// Records edit the merged data, so the parent is folded in here and the component spawns from the result.
		std::shared_ptr<desc::desc_base> parent;
		if (cnode.parent_desc.is_valid()) {
			parent = assembler.get_resource_system().require_sync<desc::desc_base>(cnode.parent_desc.get_tag()).get_sync();
		}
		std::string type = cnode.type_name;
		if (type.empty() && parent) type = std::string{ parent->get_type() };
		assembler.assemble_and_apply(reg, live, type, {}, compose_component_json(cnode, parent.get()), comp_name);
	}

	// A fresh spawn has no children yet; on a reload the live ones go away unless a node claims them.
	const bool reload = mode == reconcile_mode::reload;
	live_children existing = reload ? live_children(reg, live) : live_children();
	std::unordered_set<entt::entity> kept;

	for (const auto& child_node : content->children) {
		const uint64_t node_id = child_node.node_id;
		std::string_view name = child_node.name.empty() ? std::string_view{ "unnamed_child" } : std::string_view{ child_node.name };
		const glm::mat4 local = child_node.get_local_transform();

		entt::entity child = reload ? existing.take(node_id, name) : entt::entity{ entt::null };
		if (child != entt::null) {
			reg.get_or_emplace<scn::local_transform>(child).local = local;
			if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
				reg.ctx().get<ecs::event<scn::transform_updated>>().emit(child);
			}
			reg.emplace_or_replace<scn::name_component>(child, scn::name_component{ .name = std::string{ name } });

			apply_node_path(reg, live, child, node_id);
			reconcile(assembler, reg, child, child_node, reconcile_mode::reload);
		} else {
			child = create_child_entity(reg, live, std::string{ name }, local);
			apply_node_path(reg, live, child, node_id);
			reconcile(assembler, reg, child, child_node, reconcile_mode::spawn);
		}
		kept.insert(child);
	}

	if (!reload) return;

	// Re-fetch: spawning children above may have created the component or moved it.
	if (auto* children_comp = reg.try_get<scn::children_component>(live)) {
		auto& children_vec = children_comp->children;
		for (auto it = children_vec.begin(); it != children_vec.end(); ) {
			if (!kept.contains(*it)) {
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

namespace {

	bool is_prefab_reference_component(const json::value& comp)
	{
		if (!comp.is_object()) return false;
		const auto& obj = comp.as_object();
		const auto* type = obj.if_contains("__type");
		const auto* parent = obj.if_contains("__parent");
		return type && type->is_string() && (type->as_string() == "prefab_desc" || type->as_string() == "world_desc")
			&& parent && parent->is_string();
	}

	// Turns what an explicit nested instance layered over its prefab into records: its other components
	// (set at "": added, or merged into the prefab's component of that name) and its children (add_node: added,
	// or merged into the prefab's node with the same id).
	void upgrade_node_v2(json::object& node)
	{
		auto comps = node.find("components");
		std::string reference_key;
		if (comps != node.end() && comps->value().is_object()) {
			for (const auto& kv : comps->value().as_object()) {
				if (is_prefab_reference_component(kv.value())) {
					reference_key = std::string{ kv.key() };
					break;
				}
			}
		}

		if (reference_key.empty()) {
			if (auto children = node.find("children"); children != node.end() && children->value().is_array()) {
				for (auto& child : children->value().as_array()) {
					if (child.is_object()) upgrade_node_v2(child.as_object());
				}
			}
			return;
		}

		json::array records;
		if (auto existing = node.find("overrides"); existing != node.end() && existing->value().is_array()) {
			records = std::move(existing->value().as_array());
		}
		json::object kept;
		for (auto& kv : comps->value().as_object()) {
			if (kv.key() == reference_key) {
				kept[kv.key()] = std::move(kv.value());
				continue;
			}
			json::object record;
			record["target"] = json::array{};
			record["component"] = std::string{ kv.key() };
			record["path"] = "";
			record["value"] = std::move(kv.value());
			records.push_back(std::move(record));
		}
		comps->value() = std::move(kept);

		if (auto children = node.find("children"); children != node.end()) {
			if (children->value().is_array()) {
				for (auto& child : children->value().as_array()) {
					// an overlay child can itself be a nested instance
					if (child.is_object()) upgrade_node_v2(child.as_object());
					json::object record;
					record["target"] = json::array{};
					record["op"] = "add_node";
					record["value"] = std::move(child);
					records.push_back(std::move(record));
				}
			}
			node.erase(children);
		}

		if (!records.empty()) {
			node["overrides"] = std::move(records);
		}
	}
}

// v2 let an explicit nested instance carry a node tree that was matched against the prefab by id; v3 spells those
// changes out as `overrides` records, which can also say "back to the default value" and remove things.
void scn::migrate_prefab_v2(json::object& obj)
{
	upgrade_node_v2(obj);
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
		// An instance node keeps itself (it names the prefab again, so the changed one is picked up); a plain
		// prefab root is rebuilt from the changed desc. A copy: reconcile replaces the prefab_instance it came from.
		const scn::prefab_desc::prefab_node& remembered = reg.get<scn::prefab_instance>(root_entity).local_overrides;
		const scn::prefab_desc::prefab_node node = remembered.components.empty() ? desc->get_root() : remembered;
		scn::reconcile(assembler, reg, root_entity, node, scn::reconcile_mode::reload);
	}
}

// Declared by hand in tests/scn_prefab_merge_tests.cpp, so it needs its own export.
SNK_API void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node)
{
	scn::reconcile(assembler, reg, e, node, scn::reconcile_mode::spawn);
}

void scn::assemble_prefab(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name, res::tag source_prefab)
{
	const auto& root_node = prefab.get_root();

	// The root of a spawn is the origin of its own paths; a node spawned by a parent already has its path.
	scn::node_index::get(reg);
	if (!reg.all_of<scn::node_id_component>(e) && !reg.all_of<scn::parent_component>(e))
		reg.emplace<scn::node_id_component>(e);

	apply_root_transform_fallback(reg, e, root_node);
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

	scn::reconcile(assembler, reg, e, root_node, scn::reconcile_mode::spawn);
}