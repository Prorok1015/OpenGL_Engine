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

	// Both layers are optional; the override's transform component wins over the base's when it differs from the
	// identity value (the same "set" test the format has always used).
	glm::mat4 merged_local_transform(const prefab_node* base, const prefab_node* over)
	{
		prefab_node t;
		t.position = (over && over->position != glm::vec3(0)) ? over->position : (base ? base->position : glm::vec3(0));
		t.rotation = (over && over->rotation != glm::vec3(0)) ? over->rotation : (base ? base->rotation : glm::vec3(0));
		t.scale    = (over && over->scale != glm::vec3(1)) ? over->scale : (base ? base->scale : glm::vec3(1));
		return t.get_local_transform();
	}

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

	// A child of the base layered with the record that targets it (either may be missing).
	struct child_pair {
		const prefab_node* base = nullptr;
		const prefab_node* over = nullptr;
	};

	// Base children in order, each with its matching override child, then the override-only ones. Nodes pair by
	// node id; nodes without an id (hand-built descs) can only pair by name.
	std::vector<child_pair> pair_children(const prefab_node* base, const prefab_node* over)
	{
		std::vector<child_pair> pairs;
		const size_t base_count = base ? base->children.size() : 0;
		const size_t over_count = over ? over->children.size() : 0;
		pairs.reserve(base_count + over_count);

		std::vector<bool> consumed(over_count, false);
		std::unordered_map<uint64_t, size_t> over_by_id;
		over_by_id.reserve(over_count);
		bool over_has_idless = false;
		for (size_t i = 0; i < over_count; ++i) {
			if (over->children[i].node_id != desc::invalid_node_id)
				over_by_id.emplace(over->children[i].node_id, i);
			else over_has_idless = true;
		}

		for (size_t b = 0; b < base_count; ++b) {
			const prefab_node& base_child = base->children[b];
			size_t found = over_count;
			if (base_child.node_id != desc::invalid_node_id) {
				if (auto it = over_by_id.find(base_child.node_id); it != over_by_id.end() && !consumed[it->second])
					found = it->second;
			}
			if (found == over_count && (base_child.node_id == desc::invalid_node_id || over_has_idless)) {
				for (size_t i = 0; i < over_count; ++i) {
					const prefab_node& candidate = over->children[i];
					const bool nameable = base_child.node_id == desc::invalid_node_id || candidate.node_id == desc::invalid_node_id;
					if (!consumed[i] && nameable && candidate.name == base_child.name) {
						found = i;
						break;
					}
				}
			}

			child_pair pair{ &base_child, nullptr };
			if (found != over_count) {
				consumed[found] = true;
				pair.over = &over->children[found];
			}
			pairs.push_back(pair);
		}
		for (size_t i = 0; i < over_count; ++i) {
			if (!consumed[i]) pairs.push_back({ nullptr, &over->children[i] });
		}
		return pairs;
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
	const scn::prefab_desc::prefab_node* base,
	const scn::prefab_desc::prefab_node* records,
	reconcile_mode mode
) {
	// An explicit nested instance replaces the base with the referenced prefab; its own node keeps the records.
	res::res_handle<scn::prefab_desc> nested_asset;
	if (records) {
		for (const auto& [comp_name, cnode] : records->components) {
			if (cnode.type_name == "prefab_desc" && cnode.parent_desc.is_valid()) {
				// cnode.parent_desc is the handle captured at first assembly and goes stale; re-resolve by tag.
				nested_asset = assembler.get_resource_system().require_sync<scn::prefab_desc>(cnode.parent_desc.get_tag());
				if (ENGINE_VERIFY(nested_asset.is_ready(), "nested prefab '{}' is not loaded, ignoring", cnode.parent_desc.get_tag().view())) {
					base = &nested_asset->get_root();
				}

				reg.emplace_or_replace<scn::prefab_instance>(live, scn::prefab_instance{
					cnode.parent_desc.get_tag(),
					*records
				});
				break;
			}
		}
	}

	if (base) {
		for (const auto& [comp_name, cnode] : base->components) {
			if (cnode.type_name == "prefab_desc") continue;

			json::object final_overrides = cnode.overrides;
			if (records) {
				if (auto it = records->components.find(comp_name); it != records->components.end()) {
					for (const auto& kv : it->second.overrides)
						final_overrides[kv.key()] = kv.value();
				}
			}
			assembler.assemble_and_apply(reg, live, cnode.type_name, cnode.parent_desc, final_overrides, comp_name);
		}
	}

	if (records) {
		for (const auto& [comp_name, cnode] : records->components) {
			if (cnode.type_name == "prefab_desc") continue;
			if (!base || base->components.find(comp_name) == base->components.end()) {
				assembler.assemble_and_apply(reg, live, cnode.type_name, cnode.parent_desc, cnode.overrides, comp_name);
			}
		}
	}

	// Children the components above may have spawned (a referenced prefab's content) are not looked up on a
	// fresh spawn; on a reload they are part of the live set and go away unless a node claims them.
	const bool reload = mode == reconcile_mode::reload;
	live_children existing = reload ? live_children(reg, live) : live_children();
	std::unordered_set<entt::entity> kept;

	for (const auto& pair : pair_children(base, records)) {
		const uint64_t node_id = effective_node_id(pair.base, pair.over);
		std::string_view name = "unnamed_child";
		if (pair.over && !pair.over->name.empty()) name = pair.over->name;
		else if (pair.base) name = pair.base->name;
		const glm::mat4 local = merged_local_transform(pair.base, pair.over);

		entt::entity child = reload ? existing.take(node_id, name) : entt::entity{ entt::null };
		if (child != entt::null) {
			reg.get_or_emplace<scn::local_transform>(child).local = local;
			if (reg.ctx().contains<ecs::event<scn::transform_updated>>()) {
				reg.ctx().get<ecs::event<scn::transform_updated>>().emit(child);
			}
			reg.emplace_or_replace<scn::name_component>(child, scn::name_component{ .name = std::string{ name } });

			apply_node_path(reg, live, child, node_id);
			reconcile(assembler, reg, child, pair.base, pair.over, reconcile_mode::reload);
		} else {
			child = create_child_entity(reg, live, std::string{ name }, local);
			apply_node_path(reg, live, child, node_id);
			reconcile(assembler, reg, child, pair.base, pair.over, reconcile_mode::spawn);
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
		scn::reconcile(assembler, reg, root_entity, &desc->get_root(), &instance.local_overrides, scn::reconcile_mode::reload);
	}
}

// Declared by hand in tests/scn_prefab_merge_tests.cpp, so it needs its own export.
SNK_API void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node)
{
	scn::reconcile(assembler, reg, e, nullptr, &node, scn::reconcile_mode::spawn);
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

	scn::reconcile(assembler, reg, e, &root_node, nullptr, scn::reconcile_mode::spawn);
}