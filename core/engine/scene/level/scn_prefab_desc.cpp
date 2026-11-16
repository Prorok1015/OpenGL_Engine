#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include "scn_hierarchy_destroy.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_manifest.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/scn_glm_json_convert.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/res_system.h>
#include <snk/common/ds/ds_json_storage.hpp>
#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

		// A transform field is three numbers; anything else is reported and the default stays.
		void read_vec3(const json::object& transform, std::string_view key, glm::vec3& out, const std::string& node_name)
		{
			const auto* value = transform.if_contains(key);
			if (!value) return;
			const bool valid = value->is_array() && value->as_array().size() == 3
				&& std::ranges::all_of(value->as_array(), [] (const json::value& n) { return n.is_number(); });
			if (!valid) {
				egLOG_WARN("desc", "node '{}': transform '{}' must be three numbers, ignoring it", node_name, key);
				return;
			}
			out = json::value_to<glm::vec3>(*value);
		}

		// `owner` names the memory:// copies of inline descs, so it has to be the desc the JSON came from. `json_path` is
		// where the component is in the owner's file, for the diagnostics of a load (#595).
		prefab_comp_node parse_component(desc::desc_system& desc_system, const desc::desc_base& owner, const json::value& val,
			std::string_view explicit_type, std::string_view json_path = {})
		{
			// A component written as one reference, in either form a file writes it: an object that is {guid, path} is that
			// reference, not a component with the overrides "guid" and "path" (#920). Under the key of a registered type
			// an object stays that type's component: its fields may well be called "path".
			if (val.is_string() || (!desc_system.is_registered(explicit_type) && res::reference_from_json(val))) {
				return prefab_comp_node{
					.parent_desc = desc_system.get_field_desc<desc::desc_base>(owner, val, json_path)
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
				cnode.parent_desc = desc_system.get_field_desc<desc::desc_base>(owner, *p, std::string{ json_path } + "/__parent");
			} else if (!cnode.type_name.empty() && !desc_system.is_registered(cnode.type_name)) {
				// The scene is still built, without this component (the assembler warns again when it skips it).
				::res::report_load_warning(::res::diagnostic_kind::unknown_component, json_path,
					std::format("no component type '{}' is registered; the component is skipped", cnode.type_name));
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
		prefab_node parse_node(desc::desc_system& desc_system, const desc::desc_base& owner, const json::object& obj, bool is_root,
			std::string_view json_path = {})
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
				read_vec3(t_obj, "position", result.position, result.name);
				read_vec3(t_obj, "rotation", result.rotation, result.name);
				read_vec3(t_obj, "scale", result.scale, result.name);
			}

			if (auto const* p = obj.if_contains("components"); p && p->is_object()) {
				for (const auto& kv : p->as_object()) {
					std::string_view comp_key = kv.key();
					result.components[std::string(comp_key)] = parse_component(desc_system, owner, kv.value(), comp_key,
						std::string{ json_path } + "/components/" + ::res::json_pointer_token(comp_key));
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
					std::size_t index = 0;
					for (const auto& child : p->as_array()) {
						const std::string child_path = std::string{ json_path } + "/children/" + std::to_string(index++);
						if (!child.is_object()) {
							egLOG_WARN("desc", "node '{}': child is not an object, skipping", result.name);
							continue;
						}
						result.children.push_back(parse_node(desc_system, owner, child.as_object(), false, child_path));
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
		// A refused reference has no tag of its own (#697): the file keeps the reference it had (#952).
		if (cnode.overrides.empty() && cnode.type_name.empty() && cnode.parent_desc.is_valid()) {
			return json::value_from(::res::refused_tag(cnode.parent_desc));
		}

		json::object out;
		if (!cnode.type_name.empty()) {
			out["__type"] = cnode.type_name;
		}
		if (cnode.parent_desc.is_valid()) {
			out["__parent"] = json::value_from(::res::refused_tag(cnode.parent_desc));
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

	// What the engine keeps of a node a module or the editor handed in (#827): copies of its records and edits were
	// constructed in the caller's resource.
	void keep_in_default_resource(prefab_node& node)
	{
		for (auto& [key, component] : node.components) {
			ds::keep_in_default_resource(component.overrides);
			for (auto& edit : component.edits) ds::keep_in_default_resource(edit.value);
		}
		for (auto& record : node.overrides) ds::keep_in_default_resource(record.value);
		for (auto& child : node.children) keep_in_default_resource(child);
	}

	// Copies the JSON straight into the default resource, once. Lists every field of prefab_node: a new one goes here too.
	prefab_node kept_copy(const prefab_node& node)
	{
		prefab_node copy;
		copy.name = node.name;
		copy.node_id = node.node_id;
		copy.position = node.position;
		copy.rotation = node.rotation;
		copy.scale = node.scale;
		copy.has_transform = node.has_transform;
		for (const auto& [key, component] : node.components) {
			prefab_comp_node& kept = copy.components[key];
			kept.type_name = component.type_name;
			kept.parent_desc = component.parent_desc;
			kept.overrides = component.overrides; // assignment: into the default resource of `kept`
			kept.edits.reserve(component.edits.size());
			for (const auto& edit : component.edits) {
				kept.edits.push_back({ edit.path, json::value(edit.value, json::storage_ptr{}) });
			}
		}
		copy.overrides.reserve(node.overrides.size());
		for (const auto& record : node.overrides) {
			copy.overrides.push_back({ record.target, record.component, record.path, json::value(record.value, json::storage_ptr{}), record.op });
		}
		copy.children.reserve(node.children.size());
		for (const auto& child : node.children) {
			copy.children.push_back(kept_copy(child));
		}
		return copy;
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

	// Where one spawn is cut short: the project's prefab_limits (scn::set_spawn_limits, from scene_init).
	scn::spawn_limits g_spawn_limits;
	static_assert(scn::spawn_limits{}.nesting_depth == prj::prefab_spawn_limits{}.nesting_depth
		&& scn::spawn_limits{}.nodes_per_spawn == prj::prefab_spawn_limits{}.nodes_per_spawn);

	// What one spawn knows on its way down the tree (#924). A project's files decide what references what: a prefab
	// that instantiates itself, directly or through others, must end, and so must one that fans out without bound.
	struct spawn_walk
	{
		// The prefabs whose own content the node at hand is, outermost first; the tags live on the stack of the walk.
		std::span<const res::tag> open;
		// How many of the first ones are the file the spawn starts from (a world, a prefab spawned as such).
		std::size_t roots = 0;
		std::size_t nodes = 0;
		bool over_budget = false;
	};

	// The walk of the spawn that is running on this thread. A component whose parent is a prefab is spawned through the
	// assembler (assemble_prefab), which comes back into reconcile: that is the same spawn, not a new one.
	thread_local spawn_walk* t_walk = nullptr;

	class walk_scope
	{
	public:
		walk_scope() : m_previous(t_walk)
		{
			if (t_walk == nullptr) t_walk = &m_own;
		}
		~walk_scope() { t_walk = m_previous; }
		walk_scope(const walk_scope&) = delete;
		walk_scope& operator=(const walk_scope&) = delete;

		spawn_walk& walk() const { return *t_walk; }
		// No spawn was running on this thread: this call is the outer one.
		bool outermost() const { return m_previous == nullptr; }

	private:
		spawn_walk m_own;
		spawn_walk* m_previous;
	};

	// Says what is open for the work below it, and puts back what was, however that work ends.
	class open_scope
	{
	public:
		open_scope(spawn_walk& walk, std::span<const res::tag> open) : m_walk(walk), m_previous(walk.open) { walk.open = open; }
		~open_scope() { m_walk.open = m_previous; }
		open_scope(const open_scope&) = delete;
		open_scope& operator=(const open_scope&) = delete;

	private:
		spawn_walk& m_walk;
		std::span<const res::tag> m_previous;
	};

	bool is_open(std::span<const res::tag> tags, const res::tag& tag)
	{
		return !tag.string().empty() && std::find(tags.begin(), tags.end(), tag) != tags.end();
	}

	std::string path_text(std::span<const res::tag> open, std::span<const res::tag> chain, const res::tag& last)
	{
		std::string text;
		for (const std::span<const res::tag> part : { open, chain }) {
			for (const res::tag& tag : part) {
				text += std::string(tag.view()) + " -> ";
			}
		}
		return text + std::string(last.view());
	}

	// What stops `file` from being opened for its content: it is open already, or would be nested too deep.
	bool refuses_to_open(const spawn_walk& walk, const res::tag& file)
	{
		if (is_open(walk.open, file)) {
			egLOG_ERROR("desc", "a prefab instantiates itself, it is not built again: {}", path_text(walk.open, {}, file));
			return true;
		}
		if (walk.open.size() - std::min(walk.roots, walk.open.size()) >= g_spawn_limits.nesting_depth) {
			egLOG_ERROR("desc", "prefabs nested deeper than {} (project setting prefab_limits.nesting_depth), "
				"not built: {}",
				g_spawn_limits.nesting_depth, path_text(walk.open, {}, file));
			return true;
		}
		return false;
	}

	// A node of an expanded tree that is not content of the file its parent is from: an outer file put it there.
	struct scope_mark
	{
		// The node's child indices from the root of the tree.
		std::vector<std::size_t> path;
		// How many of the open prefabs the node is inside; its subtree is inside as many unless marked again.
		std::size_t depth = 0;
	};

	// Marks sorted by path, seen from the node `skip` levels below the root they were made for.
	struct marks_view
	{
		std::span<const scope_mark> marks;
		std::size_t skip = 0;

		const scope_mark* here() const
		{
			return !marks.empty() && marks.front().path.size() == skip ? &marks.front() : nullptr;
		}

		marks_view child(std::size_t index) const
		{
			const std::span<const scope_mark> below = here() ? marks.subspan(1) : marks;
			const auto less = [at = skip] (const scope_mark& mark, std::size_t value) { return mark.path[at] < value; };
			const auto first = std::lower_bound(below.begin(), below.end(), index, less);
			const auto last = std::lower_bound(first, below.end(), index + 1, less);
			return marks_view{
				below.subspan(static_cast<std::size_t>(first - below.begin()), static_cast<std::size_t>(last - first)), skip + 1 };
		}
	};

	// The marks at and below `from` become marks of a tree where the children of `from` start at `first_child`.
	void graft_marks(std::vector<scope_mark>& marks, marks_view from, std::size_t first_child, std::size_t deepest)
	{
		for (const scope_mark& mark : from.marks) {
			if (mark.path.size() <= from.skip) continue;
			scope_mark moved{
				std::vector<std::size_t>(mark.path.begin() + static_cast<std::ptrdiff_t>(from.skip), mark.path.end()),
				std::min(mark.depth, deepest) };
			moved.path.front() += first_child;
			marks.push_back(std::move(moved));
		}
	}

	// Child `index` of the node at `parent` is removed: its marks go, those of the later children move up.
	void forget_child(std::vector<scope_mark>& marks, std::span<const std::size_t> parent, std::size_t index)
	{
		const auto below_parent = [parent] (const scope_mark& mark) {
			return mark.path.size() > parent.size() && std::equal(parent.begin(), parent.end(), mark.path.begin());
		};
		std::erase_if(marks, [&] (const scope_mark& mark) { return below_parent(mark) && mark.path[parent.size()] == index; });
		for (scope_mark& mark : marks) {
			if (below_parent(mark) && mark.path[parent.size()] > index) --mark.path[parent.size()];
		}
	}

	// What expanding one node gave besides the tree: the prefabs it opened, outermost first, and the marks of the tree.
	struct expansion
	{
		std::vector<res::tag> chain;
		std::vector<scope_mark> marks;
	};

	// Where add_node records put their nodes: each is content of the file the records are from.
	struct added_nodes
	{
		std::vector<scope_mark>& marks;
		// The node whose children are being added to.
		std::vector<std::size_t> at;
		std::size_t depth = 0;
	};

	struct prefab_ref {
		std::string key;
		std::shared_ptr<const scn::prefab_desc> prefab;
	};

	// The component of `node` that makes it an instance of a prefab: a typed { "__parent": X } component or a
	// plain string reference that resolves to a prefab. Looked up by tag every time, a handle kept in the node
	// goes stale when the prefab is reloaded.
	std::optional<prefab_ref> find_prefab_ref(const scn::ecs_assembler& assembler, const prefab_node& node)
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

	void add_or_merge_node(desc::desc_system& desc_system, const desc::desc_base& owner, std::vector<prefab_node>& siblings,
		const json::object& value, added_nodes& added);

	// Overlays `src` (the JSON of a node) onto an existing node: only what the JSON spells out changes.
	void merge_into_node(desc::desc_system& desc_system, const desc::desc_base& owner, prefab_node& dst, const json::object& src,
		added_nodes& added)
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
				it->second.edits.push_back({ "", json::value(json::object(comp.overrides, json::storage_ptr{})) });
			}
		}
		dst.overrides.insert(dst.overrides.end(), std::make_move_iterator(parsed.overrides.begin()), std::make_move_iterator(parsed.overrides.end()));
		if (auto* children = src.if_contains("children"); children && children->is_array()) {
			for (const auto& child : children->as_array()) {
				if (child.is_object()) add_or_merge_node(desc_system, owner, dst.children, child.as_object(), added);
			}
		}
	}

	void add_or_merge_node(desc::desc_system& desc_system, const desc::desc_base& owner, std::vector<prefab_node>& siblings,
		const json::object& value, added_nodes& into)
	{
		std::optional<uint64_t> id;
		if (const auto* raw = value.if_contains("__id"); raw && raw->is_string()) {
			id = desc::parse_node_id(raw->as_string());
		}
		if (id) {
			if (auto existing = std::ranges::find(siblings, *id, &prefab_node::node_id); existing != siblings.end()) {
				into.at.push_back(static_cast<std::size_t>(existing - siblings.begin()));
				merge_into_node(desc_system, owner, *existing, value, into);
				into.at.pop_back();
				return;
			}
		}
		scope_mark mark{ into.at, into.depth };
		mark.path.push_back(siblings.size());
		into.marks.push_back(std::move(mark));
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
		// The component's type decides, not the record's key: a typed component may well have a field called "path".
		const prefab_comp_node& held = it->second;
		const bool has_fields = !held.type_name.empty() && (desc_system.is_registered(held.type_name) || held.parent_desc.is_valid());
		const bool whole_reference = record.value.is_string() || (!has_fields && res::reference_from_json(record.value));
		if (record.path.empty() && whole_reference) {
			// a plain reference to a desc replaces the component whole
			it->second = scn::parse_component(desc_system, owner, record.value, record.component);
			return;
		}
		if (record.path.empty() && !record.value.is_object()) {
			egLOG_WARN("desc", "override: component '{}' can only be merged with an object at ''", record.component);
			return;
		}
		it->second.edits.push_back({ record.path, json::value(record.value, json::storage_ptr{}) });
	}

	// Applies the records to a tree that is already a private copy. A record that targets a node below an
	// instance of another prefab is handed down to that instance, which resolves it once it is expanded.
	// `marks` follows what the records do to the tree; a node they add is inside `depth_of_records` open prefabs.
	void apply_records(const scn::ecs_assembler& assembler, const desc::desc_base& owner, prefab_node& root,
		const std::vector<scn::override_record>& records, std::vector<scope_mark>& marks, std::size_t depth_of_records)
	{
		auto& desc_system = assembler.get_desc_system();
		for (const auto& record : records) {
			prefab_node* parent = nullptr;
			prefab_node* node = &root;
			added_nodes added{ marks, {}, depth_of_records };
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
				added.at.push_back(static_cast<std::size_t>(child - node->children.begin()));
				parent = node;
				node = &*child;
			}
			if (!resolved) continue;

			switch (record.op) {
			case scn::override_op::add_node:
				add_or_merge_node(desc_system, owner, node->children, record.value.as_object(), added);
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
					const std::size_t index = added.at.back();
					added.at.pop_back();
					forget_child(marks, added.at, index);
					std::erase_if(parent->children, [node] (const prefab_node& candidate) { return &candidate == node; });
				}
				break;

			case scn::override_op::set:
				apply_set_record(desc_system, owner, *node, record);
				break;
			}
		}
	}

	// Stands for the file records are written in: get_field_desc reads no more of an owner than its tag.
	class written_in_file final : public scn::prefab_desc
	{
	public:
		explicit written_in_file(const res::tag& file) { set_tag(file); }
	};

	// What `node` stands for when it is more than its own fields: the prefab it references (itself expanded)
	// plus the node's own extra components and children, then the node's override records. False for a plain
	// node without records. `source` is the referenced prefab, empty when there is none.
	// `open`: the prefabs `node` is inside (spawn_walk::open), the first `roots` of them the file the spawn starts from;
	// `own`: the marks of the tree `node` is in, seen from it. A reference to a prefab that is open already, or one
	// nested too deep, is ignored: the node is built without it and without the component that made it.
	// Only the root of a prefab is inside it here: what `node` attaches below itself, and what its records add, is
	// content of the file `node` is from, so `made.marks` says for each such node of `out` how much it is inside.
	bool materialize(const scn::ecs_assembler& assembler, const prefab_node& node, prefab_node& out, res::tag& source,
		std::span<const res::tag> open, std::size_t roots, expansion& made, marks_view own)
	{
		// Taken before this call opens anything: `node` is content of a file that is inside this much.
		const std::size_t depth = open.size() + made.chain.size();
		// That file, the last one open (none for a node handed in alone): its records are written there (#952).
		const bool file_known = !made.chain.empty() || !open.empty();
		const res::tag written_in = !made.chain.empty() ? made.chain.back() : (file_known ? open.back() : res::tag{});
		std::optional<prefab_ref> ref = find_prefab_ref(assembler, node);
		std::optional<std::string> ignored;
		if (ref) {
			const res::tag& tag = ref->prefab->get_tag();
			if (is_open(open, tag) || is_open(made.chain, tag)) {
				egLOG_ERROR("desc", "a prefab instantiates itself, reference ignored: {}", path_text(open, made.chain, tag));
				ignored = ref->key;
			} else if (depth - std::min(roots, depth) >= g_spawn_limits.nesting_depth) {
				egLOG_ERROR("desc", "prefabs nested deeper than {} (project setting prefab_limits.nesting_depth), "
					"reference ignored: {}",
					g_spawn_limits.nesting_depth, path_text(open, made.chain, tag));
				ignored = ref->key;
			}
			if (ignored) ref.reset();
		}
		if (!ref && !ignored && node.overrides.empty()) return false;

		// The records may instantiate the prefab `node` instantiates: only the file they are written in refers to itself.
		const written_in_file file_owner(written_in);
		const desc::desc_base* owner = &file_owner;
		// Only for a call that names no file (the old reconcile / expand_instance, lower override_edit nodes, #995):
		// the instance's prefab, so a record that instantiates it again is refused (#952).
		if (ref && !file_known) owner = ref->prefab.get();
		if (ref) {
			source = ref->prefab->get_tag();
			made.chain.push_back(ref->prefab->get_tag());
			if (!materialize(assembler, ref->prefab->get_root(), out, source, open, roots, made, {})) {
				out = ref->prefab->get_root();
			}
			source = ref->prefab->get_tag();
			for (const auto& [key, comp] : node.components) {
				if (key != ref->key) out.components[key] = comp;
			}
			const std::size_t first_own = out.children.size();
			out.children.insert(out.children.end(), node.children.begin(), node.children.end());
			graft_marks(made.marks, own, first_own, depth);
			for (std::size_t i = 0; i < node.children.size(); ++i) {
				if (!own.child(i).here()) made.marks.push_back(scope_mark{ { first_own + i }, depth });
			}
		} else {
			out = node;
			graft_marks(made.marks, own, 0, depth);
			// Left in, the component would be spawned as a prefab by the assembler: the very instance just refused.
			if (ignored) out.components.erase(*ignored);
		}
		out.overrides.clear();
		apply_records(assembler, *owner, out, node.overrides, made.marks, depth);
		return true;
	}

	// serialize(parent) with the component's own overrides merged in, then the edits in order.
	json::object compose_json(const prefab_comp_node& cnode, const desc::desc_base* parent)
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

	// Live children of a node, claimed by node id (their node_id_component) or, for nodes that carry no id, by name.
	class live_children {
	public:
		live_children() = default;

		live_children(entt::registry& reg, entt::entity parent)
		{
			auto* children_comp = reg.try_get<scn::children_component>(parent);
			if (!children_comp) return;
			for (auto child : children_comp->children) {
				if (!reg.valid(child)) continue; // destroyed by game code, still listed (#543): unclaimed, a reload drops it
				if (reg.all_of<scn::spawned_root_component>(child)) continue; // game code's, not the prefab's (#551)
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

json::object scn::compose_component_json(const prefab_desc::prefab_comp_node& cnode, const desc::desc_base* parent)
{
	return compose_json(cnode, parent);
}

bool scn::expand_instance(
	const ecs_assembler& assembler, const prefab_desc::prefab_node& node, prefab_desc::prefab_node& out, res::tag& source)
{
	return expand_instance(assembler, node, out, source, res::tag{});
}

bool scn::expand_instance(const ecs_assembler& assembler, const prefab_desc::prefab_node& node, prefab_desc::prefab_node& out,
	res::tag& source, const res::tag& file)
{
	expansion made;
	// A known file is the one the expansion starts from, as a spawn's root file is.
	const bool known = !file.string().empty();
	const std::span<const res::tag> open = known ? std::span<const res::tag>(&file, 1) : std::span<const res::tag>{};
	if (!materialize(assembler, node, out, source, open, known ? 1 : 0, made, {})) return false;
	keep_in_default_resource(out);
	return true;
}

scn::spawn_limits scn::set_spawn_limits(const spawn_limits& limits)
{
	return std::exchange(g_spawn_limits, limits);
}

namespace {

void reconcile_node(const scn::ecs_assembler& assembler, entt::registry& reg, entt::entity live, const prefab_node& node,
	scn::reconcile_mode mode, spawn_walk& walk, marks_view own);

}

void scn::reconcile(
	const scn::ecs_assembler& assembler,
	entt::registry& reg,
	entt::entity live,
	const scn::prefab_desc::prefab_node& node,
	reconcile_mode mode
) {
	reconcile(assembler, reg, live, node, mode, res::tag{});
}

void scn::reconcile(
	const scn::ecs_assembler& assembler,
	entt::registry& reg,
	entt::entity live,
	const scn::prefab_desc::prefab_node& node,
	reconcile_mode mode,
	const res::tag& file
) {
	const walk_scope scope;
	spawn_walk& walk = scope.walk();
	if (file.string().empty()) {
		reconcile_node(assembler, reg, live, node, mode, walk, {});
		return;
	}
	// `node` is content of `file`, which is open while it is built, as assemble_prefab opens its prefab (#993).
	if (refuses_to_open(walk, file)) return;
	std::vector<res::tag> inside(walk.open.begin(), walk.open.end());
	inside.push_back(file);
	if (scope.outermost()) walk.roots = 1;
	const open_scope opened(walk, inside);
	reconcile_node(assembler, reg, live, node, mode, walk, {});
}

void scn::spawn_world(
	const scn::ecs_assembler& assembler,
	entt::registry& reg,
	entt::entity e,
	const scn::world_desc& desc,
	const std::string_view name,
	const res::tag& file
) {
	// A desc that carries the file as its own tag opens it itself, as a level load does; opened here too,
	// its own spawn would be refused as a self-reference and leave the world empty.
	if (file.string().empty() || desc.get_tag() == file) {
		assembler.spawn_from_desc(reg, e, desc, name);
		return;
	}
	const walk_scope scope;
	spawn_walk& walk = scope.walk();
	// Opened as reconcile opens a known file; the world spawner's assemble_prefab gets no tag and opens nothing more.
	if (refuses_to_open(walk, file)) return;
	std::vector<res::tag> inside(walk.open.begin(), walk.open.end());
	inside.push_back(file);
	if (scope.outermost()) walk.roots = 1;
	const open_scope opened(walk, inside);
	assembler.spawn_from_desc(reg, e, desc, name);
}

namespace {

// `own`: the marks of the expanded tree `node` is a part of, seen from it; walk.open is what `node` is inside.
void reconcile_node(const scn::ecs_assembler& assembler, entt::registry& reg, entt::entity live, const prefab_node& node,
	scn::reconcile_mode mode, spawn_walk& walk, marks_view own)
{
	using scn::reconcile_mode;
	++walk.nodes;
	// `node` itself unless it references a prefab or carries override records; `node` may be a part of this
	// entity's prefab_instance, which is replaced below, so nothing reads it after that.
	prefab_node expanded;
	const prefab_node* content = &node;
	res::tag source;
	expansion made;
	marks_view marks = own;
	// What the content is inside: the prefabs `node` is inside and, after them, the ones it stands for.
	std::vector<res::tag> opened_here;
	std::span<const res::tag> inside = walk.open;
	if (materialize(assembler, node, expanded, source, walk.open, walk.roots, made, own)) {
		std::ranges::sort(made.marks, {}, &scope_mark::path);
		marks = marks_view{ made.marks, 0 };
		if (!made.chain.empty()) {
			opened_here.assign(walk.open.begin(), walk.open.end());
			opened_here.insert(opened_here.end(), made.chain.begin(), made.chain.end());
			inside = opened_here;
		}
		if (!source.string().empty()) {
			// A transform the node spells out places the instance; otherwise the prefab root's does, with the
			// target-[] transform records applied. Taken before the prefab_instance below replaces `node`.
			const glm::mat4 placement = (node.has_transform ? node : expanded).get_local_transform();
			// Where game code moved a spawned tree is the game's state: a reload of its prefab keeps it (#551).
			const bool kept_by_game = mode == reconcile_mode::reload && reg.all_of<scn::spawned_root_component, scn::local_transform>(live);
			if (!kept_by_game && (!node.has_transform || !reg.all_of<scn::local_transform>(live))) {
				reg.emplace_or_replace<scn::local_transform>(live, placement);
			}
			// `node` is content of the last file open on entry, not of the prefabs it opens itself.
			const res::tag content_of = walk.open.empty() ? res::tag{} : walk.open.back();
			reg.emplace_or_replace<scn::prefab_instance>(live, scn::prefab_instance{ source, kept_copy(node), content_of });
			emit_event<scn::transform_updated>(reg, live);
		}
		content = &expanded;
	}
	// Open for the components of this node; each child says below how much of it it is inside.
	const open_scope opened(walk, inside);

	for (const auto& [comp_name, cnode] : content->components) {
		if (cnode.type_name == "prefab_desc") continue;

		if (cnode.edits.empty()) {
			assembler.assemble_and_apply(reg, live, cnode.type_name, cnode.parent_desc, cnode.overrides, comp_name);
			continue;
		}

		// Records edit the merged data, so the parent is folded in here and the component spawns from the result.
		std::shared_ptr<desc::desc_base> parent;
		if (cnode.parent_desc.is_valid() && !cnode.parent_desc.has_error() && cnode.parent_desc.get_tag().is_valid()) {
			parent = assembler.get_resource_system().require_sync<desc::desc_base>(cnode.parent_desc.get_tag()).get_sync();
		}
		std::string type = cnode.type_name;
		if (type.empty() && parent) type = std::string{ parent->get_type() };
		assembler.assemble_and_apply(reg, live, type, {}, compose_json(cnode, parent.get()), comp_name);
	}

	// A fresh spawn has no children yet; on a reload the live ones go away unless a node claims them.
	const bool reload = mode == reconcile_mode::reload;
	live_children existing = reload ? live_children(reg, live) : live_children();
	std::unordered_set<entt::entity> kept;

	for (std::size_t index = 0; index < content->children.size(); ++index) {
		const prefab_node& child_node = content->children[index];
		if (walk.nodes >= g_spawn_limits.nodes_per_spawn) {
			if (!walk.over_budget) {
				walk.over_budget = true;
				egLOG_ERROR("desc", "a spawn builds more than {} nodes (project setting prefab_limits.nodes_per_spawn): "
					"the rest of the tree is left out{} (last prefab: {})",
					g_spawn_limits.nodes_per_spawn, reload ? ", other live instances are left as they are" : "",
					inside.empty() ? std::string_view{ "none" } : inside.back().view());
			}
			break;
		}
		// A child an outer file put here is content of that file: the prefabs opened since are not open for it.
		const marks_view child_marks = marks.child(index);
		const scope_mark* outer = child_marks.here();
		const open_scope child_open(walk, outer ? inside.first(std::min(outer->depth, inside.size())) : inside);
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

			scn::apply_node_path(reg, live, child, node_id);
			// As a fresh node has none: the reference that made it an instance may be gone, or refused now.
			reg.remove<scn::prefab_instance>(child);
			reconcile_node(assembler, reg, child, child_node, reconcile_mode::reload, walk, child_marks);
		} else {
			child = create_child_entity(reg, live, std::string{ name }, local);
			scn::apply_node_path(reg, live, child, node_id);
			reconcile_node(assembler, reg, child, child_node, reconcile_mode::spawn, walk, child_marks);
		}
		kept.insert(child);
	}

	// A walk cut short did not reach every node: a live child it never came to is not one the tree dropped.
	if (!reload || walk.over_budget) return;

	// Re-fetch: spawning children above may have created the component or moved it.
	if (auto* children_comp = reg.try_get<scn::children_component>(live)) {
		auto& children_vec = children_comp->children;
		for (auto it = children_vec.begin(); it != children_vec.end(); ) {
			const bool spawned = reg.valid(*it) && reg.all_of<scn::spawned_root_component>(*it);
			if (!kept.contains(*it) && !spawned) {
				scn::detail::destroy_hierarchy(reg, *it);
				it = children_vec.erase(it);
			} else {
				++it;
			}
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

namespace {
	// The keys the camera components of the tree live under (override records name a component by its key, and a camera of
	// an instance's prefab is not in this tree), and the two names the editor and the files give it.
	void collect_camera_keys(const json::object& node, std::unordered_set<std::string>& keys)
	{
		if (const auto components = node.find("components"); components != node.end() && components->value().is_object()) {
			for (const auto& member : components->value().as_object()) {
				if (!member.value().is_object()) continue;
				const auto* type = member.value().as_object().if_contains("__type");
				if (type && type->is_string() && type->as_string() == scn::camera_desc::__type) keys.emplace(member.key());
			}
		}
		if (const auto children = node.find("children"); children != node.end() && children->value().is_array()) {
			for (const auto& child : children->value().as_array()) {
				if (child.is_object()) collect_camera_keys(child.as_object(), keys);
			}
		}
		if (const auto records = node.find("overrides"); records != node.end() && records->value().is_array()) {
			for (const auto& record : records->value().as_array()) {
				if (!record.is_object()) continue;
				// a record that says what it sets: a camera
				const auto* component = record.as_object().if_contains("component");
				if (const auto* set = record.as_object().if_contains("value"); component && component->is_string() && set && set->is_object()) {
					const auto* type = set->as_object().if_contains("__type");
					if (type && type->is_string() && type->as_string() == scn::camera_desc::__type) keys.emplace(component->as_string());
				}
				const auto added = record.as_object().find("value");
				const auto* op = record.as_object().if_contains("op");
				if (added != record.as_object().end() && added->value().is_object() && op && op->is_string() && op->as_string() == "add_node") {
					collect_camera_keys(added->value().as_object(), keys);
				}
			}
		}
	}

	bool has_vec3(const json::object& object, std::string_view key)
	{
		const auto* value = object.if_contains(key);
		return value && value->is_array() && value->as_array().size() == 3;
	}

	// A camera record that sets camera fields ("path": "", the value is merged onto the camera of the prefab; the editor writes
	// a diff without "__type", a file may carry a whole typed camera) and has the legacy "transform" in it.
	bool is_legacy_camera_record_value(const json::value& value)
	{
		if (!value.is_object()) return false;
		const auto& object = value.as_object();
		const auto* type = object.if_contains("__type");
		if (type && (!type->is_string() || type->as_string() != scn::camera_desc::__type)) return false;
		if (object.contains("__parent") || !object.contains("transform")) return false;
		const auto* version = object.if_contains("__version");
		return !version || (version->is_int64() && version->as_int64() < 2) || (version->is_uint64() && version->as_uint64() < 2);
	}

	// The override records of a nested instance on a camera (see migrate_camera_transform_into_node), only those of a
	// component whose key is a camera's:
	// - a record at "" with the legacy camera_desc.transform in it: when the record sets the whole pose (position and
	//   rotation) the pose that was rendered becomes the `transform` records of the same target, after this one (so a
	//   transform record the instance already has, further down, wins); a partial pose cannot be converted (it is merged onto
	//   a pose in the prefab, which is not loaded here) and is dropped with a warning; fields set besides stay;
	// - a record into camera_desc.transform itself ("/transform/position") is dropped with a warning, for the same reason.
	using warn_fn = std::function<void(std::string)>;

	void migrate_camera_records(json::object& node, const std::unordered_set<std::string>& camera_keys, const warn_fn& warn)
	{
		const auto records = node.find("overrides");
		if (records == node.end() || !records->value().is_array()) return;

		json::array migrated;
		for (auto& record : records->value().as_array()) {
			if (!record.is_object()) {
				migrated.push_back(std::move(record));
				continue;
			}
			json::object& object = record.as_object();
			const auto* component = object.if_contains("component");
			const auto* path = object.if_contains("path");
			const std::string key = component && component->is_string() ? std::string{ component->as_string() } : std::string{};
			const std::string where = path && path->is_string() ? std::string{ path->as_string() } : std::string{};
			const auto* typed_value = object.if_contains("value");
			const auto* typed_type = typed_value && typed_value->is_object() ? typed_value->as_object().if_contains("__type") : nullptr;
			const bool typed_camera = typed_type && typed_type->is_string() && typed_type->as_string() == scn::camera_desc::__type;
			if (key == scn::transform_pseudo_component || (!typed_camera && !camera_keys.contains(key))) {
				migrated.push_back(std::move(record));
				continue;
			}

			const auto* value = object.if_contains("value");
			if (where.empty() && value && is_legacy_camera_record_value(*value)) {
				json::object camera = value->as_object();
				const json::value legacy = camera.at("transform");
				camera.erase("transform");
				const bool typed = camera.contains("__type");
				if (typed) camera["__version"] = 2;

				const json::value target = object.if_contains("target") ? *object.if_contains("target") : json::value(json::array{});
				std::vector<json::object> moved;
				if (legacy.is_object() && has_vec3(legacy.as_object(), "position") && has_vec3(legacy.as_object(), "rotation")) {
					json::object holder;
					holder["components"] = json::object{ { "camera", json::object{ { "__type", std::string{ scn::camera_desc::__type } }, { "transform", legacy } } } };
					scn::migrate_camera_transform_into_node(holder, warn);
					const json::object& pose = holder.at("transform").as_object();
					for (const char* field : { "position", "rotation" }) {
						json::object entry;
						entry["target"] = target;
						entry["component"] = std::string{ scn::transform_pseudo_component };
						entry["path"] = std::string("/") + field;
						entry["value"] = pose.at(field);
						moved.push_back(std::move(entry));
					}
				} else {
					warn(std::format("an override of '{}' sets no whole legacy camera_desc.transform: it is dropped, the transform of the "
						"node is the pose now (a partial pose cannot be converted here, the rest of it is in the prefab)", key));
				}

				// what is left of the record: a typed record may create the camera component, so it stays; a diff stays when it
				// still sets a field
				if (typed || std::ranges::any_of(camera, [](const auto& member) { return !member.key().starts_with("__"); })) {
					object["value"] = std::move(camera);
					migrated.push_back(std::move(record));
				}
				for (auto& entry : moved) migrated.push_back(std::move(entry));
				continue;
			}
			if (where == "/transform" || where.starts_with("/transform/")) {
				warn(std::format("an override of '{}' at '{}' is dropped: camera_desc has no transform any more, the transform of the node is the "
					"pose (it cannot be converted here, the pose it edits is in the prefab)", key, where));
				continue;
			}
			migrated.push_back(std::move(record));
		}
		records->value() = std::move(migrated);
	}

	// Every node of the tree: its children and the nodes an `add_node` record carries.
	void bake_camera_poses(json::object& node, const std::unordered_set<std::string>& camera_keys, const warn_fn& warn)
	{
		scn::migrate_camera_transform_into_node(node, warn);
		migrate_camera_records(node, camera_keys, warn);
		if (auto children = node.find("children"); children != node.end() && children->value().is_array()) {
			for (auto& child : children->value().as_array()) {
				if (child.is_object()) bake_camera_poses(child.as_object(), camera_keys, warn);
			}
		}
		if (auto records = node.find("overrides"); records != node.end() && records->value().is_array()) {
			for (auto& record : records->value().as_array()) {
				if (!record.is_object()) continue;
				auto added = record.as_object().find("value");
				const auto op = record.as_object().if_contains("op");
				if (added != record.as_object().end() && added->value().is_object() && op && op->is_string() && op->as_string() == "add_node") {
					bake_camera_poses(added->value().as_object(), camera_keys, warn);
				}
			}
		}
	}
}

// v4: a camera takes the transform of its node; the pose it kept in camera_desc.transform is written into the node.
namespace {
	void bake_prefab_camera_poses(json::object& obj, const warn_fn& warn)
	{
		std::unordered_set<std::string> camera_keys{ std::string{ "camera" }, std::string{ scn::camera_desc::__type } };
		collect_camera_keys(obj, camera_keys);
		bake_camera_poses(obj, camera_keys, warn);
	}
}

void scn::migrate_prefab_v3(json::object& obj)
{
	bake_prefab_camera_poses(obj, [](std::string message) { egLOG_WARN("desc", "{}", message); });
}

void scn::migrate_prefab_v3(json::object& obj, desc::migration_context& context)
{
	bake_prefab_camera_poses(obj, [&context](std::string message) { context.warn(std::move(message)); });
}

namespace {
	using id_remap = std::unordered_map<uint64_t, uint64_t>;

	void assign_new_ids(scn::prefab_desc::prefab_node& node, id_remap& remap) {
		const uint64_t fresh = desc::new_node_id();
		// Hand-built nodes may have no id; mapping 0 would hand all of them the same new id.
		if (node.node_id != desc::invalid_node_id) remap.insert_or_assign(node.node_id, fresh);
		node.node_id = fresh;
		for (auto& child : node.children) {
			assign_new_ids(child, remap);
		}
	}

	// Gives the nodes of an add_node value fresh ids (the copy must not share them) and records old -> new.
	void remap_added_node(json::value& value, id_remap& remap) {
		if (!value.is_object()) return;
		auto& obj = value.as_object();
		if (auto* raw = obj.if_contains("__id"); raw && raw->is_string()) {
			if (auto old_id = desc::parse_node_id(raw->as_string())) {
				const uint64_t fresh = remap.emplace(*old_id, desc::new_node_id()).first->second;
				*raw = desc::format_node_id(fresh);
			}
		}
		if (auto* kids = obj.if_contains("children"); kids && kids->is_array()) {
			for (auto& kid : kids->as_array()) remap_added_node(kid, remap);
		}
	}

	// Only ids that belong to the copied subtree change; ids of the instantiated prefab's content stay.
	void remap_overrides(scn::prefab_desc::prefab_node& node, id_remap& remap) {
		for (auto& record : node.overrides) {
			if (record.op == scn::override_op::add_node) remap_added_node(record.value, remap);
		}
		for (auto& record : node.overrides) {
			for (auto& id : record.target) {
				if (auto it = remap.find(id); it != remap.end()) id = it->second;
			}
		}
		for (auto& child : node.children) {
			remap_overrides(child, remap);
		}
	}
}

void scn::regenerate_node_ids(scn::prefab_desc::prefab_node& node) {
	id_remap remap;
	assign_new_ids(node, remap);
	remap_overrides(node, remap);
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

	// One walk for the whole call: the live instances of a prefab share one node budget, however many there are.
	const walk_scope scope;
	spawn_walk& walk = scope.walk();

	for (auto root_entity : roots_to_reload) {
		// The reload of an instance above it may have destroyed this one, or refused the reference that made it one.
		if (!reg.valid(root_entity)) continue;
		const auto* instance = reg.try_get<scn::prefab_instance>(root_entity);
		if (!instance || instance->source_prefab != desc->get_tag()) continue;
		// Out of budget: a tree not started keeps all its live children, as the rest of a tree cut short does.
		if (walk.over_budget) break; // the cut inside a tree already reported it, with this outcome
		if (walk.nodes >= g_spawn_limits.nodes_per_spawn) {
			egLOG_ERROR("desc", "a hot reload builds more than {} nodes "
				"(project setting prefab_limits.nodes_per_spawn): "
				"the other live instances of {} are left as they are",
				g_spawn_limits.nodes_per_spawn, desc->get_tag().view());
			walk.over_budget = true;
			break;
		}
		// An instance node keeps itself (it names the prefab again, so the changed one is picked up); a plain
		// prefab root is rebuilt from the changed desc. A copy: reconcile replaces the prefab_instance it came from.
		const scn::prefab_desc::prefab_node& remembered = instance->local_overrides;
		const bool plain_root = remembered.components.empty();
		const scn::prefab_desc::prefab_node node = plain_root ? desc->get_root() : remembered;
		// An instance node is content of the file it was built in: open again, its records may instantiate the prefab (#993).
		const res::tag content_of = plain_root ? res::tag{} : instance->content_of;
		const bool file_known = !content_of.string().empty();
		if (file_known && refuses_to_open(walk, content_of)) continue;

		// A plain root is the changed prefab's own content, as in assemble_prefab; an instance node opens it itself.
		std::vector<res::tag> inside(walk.open.begin(), walk.open.end());
		if (plain_root) inside.push_back(desc->get_tag());
		if (file_known) inside.push_back(content_of);
		if (scope.outermost()) walk.roots = (plain_root || file_known) ? 1 : 0;
		const open_scope opened(walk, inside);
		reconcile_node(assembler, reg, root_entity, node, scn::reconcile_mode::reload, walk, {});
	}
}

// Declared by hand in tests/scn_prefab_merge_tests.cpp, so it needs its own export.
SNK_API void assemble_prefab_node(scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const scn::prefab_desc::prefab_node& node)
{
	scn::reconcile(assembler, reg, e, node, scn::reconcile_mode::spawn);
}

void scn::assemble_prefab(const scn::ecs_assembler& assembler, entt::registry& reg, entt::entity e, const prefab_desc& prefab, const std::string_view name, res::tag source_prefab)
{
	// The prefab is open while its content is built: a node in it that instantiates it again is refused. Reached through
	// the assembler too, for a component whose parent is a prefab, and that one may be open already: then nothing of
	// the entity is touched.
	const walk_scope scope;
	spawn_walk& walk = scope.walk();
	if (refuses_to_open(walk, source_prefab)) return;

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
	// A node spawned by a parent already has its name; `name` is then only the component key (#172).
	if (!name.empty() && !reg.all_of<scn::name_component>(e)) {
		reg.emplace<scn::name_component>(e, scn::name_component{ .name = std::string(name) });
	}
	reg.emplace_or_replace<scn::depth_level>(e);

	if (!source_prefab.string().empty()) {
		reg.emplace_or_replace<scn::prefab_instance>(e, scn::prefab_instance{
			source_prefab,
			scn::prefab_desc::prefab_node{},
			res::tag{}
		});
	}

	if (reg.ctx().contains<ecs::event<scn::hierarchy_updated>>()) {
		reg.ctx().get<ecs::event<scn::hierarchy_updated>>().emit(e);
	} else {
		ecs::event<scn::hierarchy_updated> event;
		event.emit(e);
		reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>(std::move(event));
	}

	std::vector<res::tag> inside(walk.open.begin(), walk.open.end());
	if (!source_prefab.string().empty()) {
		inside.push_back(source_prefab);
		// The file an outer call starts from (a world, a prefab spawned as such) is not nested in anything.
		if (scope.outermost()) walk.roots = 1;
	}
	const open_scope opened(walk, inside);
	reconcile_node(assembler, reg, e, root_node, scn::reconcile_mode::spawn, walk, {});
}