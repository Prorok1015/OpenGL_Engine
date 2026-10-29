#include "edt_scene_history.h"
#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>

namespace {

	using prefab_node = scn::prefab_desc::prefab_node;

	bool same_records(const std::vector<scn::override_record>& a, const std::vector<scn::override_record>& b)
	{
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (a[i].target != b[i].target || a[i].component != b[i].component || a[i].path != b[i].path
				|| a[i].op != b[i].op || a[i].value != b[i].value) {
				return false;
			}
		}
		return true;
	}

	struct node_place
	{
		std::vector<prefab_node>* siblings = nullptr;
		std::size_t index = 0;
	};

	bool find_place(std::vector<prefab_node>& nodes, std::uint64_t id, node_place& out)
	{
		for (std::size_t i = 0; i < nodes.size(); ++i) {
			if (nodes[i].node_id == id) {
				out = { &nodes, i };
				return true;
			}
			if (find_place(nodes[i].children, id, out)) {
				return true;
			}
		}
		return false;
	}

	const prefab_node* find_node(const prefab_node& root, std::uint64_t id)
	{
		if (root.node_id == id) {
			return &root;
		}
		for (const auto& child : root.children) {
			if (const auto* found = find_node(child, id)) {
				return found;
			}
		}
		return nullptr;
	}

	prefab_node* find_node(prefab_node& root, std::uint64_t id)
	{
		if (root.node_id == id) {
			return &root;
		}
		node_place place;
		return find_place(root.children, id, place) ? &(*place.siblings)[place.index] : nullptr;
	}

	// Puts `node` where `state` has it: under its parent, right after its previous sibling (first when there is none).
	void insert_at(prefab_node& root, prefab_node node, const edt::scene_node_state& state)
	{
		prefab_node* parent = find_node(root, state.parent);
		auto& siblings = parent ? parent->children : root.children;
		auto at = siblings.begin();
		if (state.previous != 0) {
			const auto previous = std::find_if(siblings.begin(), siblings.end(), [&](const prefab_node& n) { return n.node_id == state.previous; });
			at = previous == siblings.end() ? siblings.end() : previous + 1;
		}
		siblings.insert(at, std::move(node));
	}

	void assign_shallow(prefab_node& to, const prefab_node& from)
	{
		to.name = from.name;
		to.node_id = from.node_id;
		to.position = from.position;
		to.rotation = from.rotation;
		to.scale = from.scale;
		to.has_transform = from.has_transform;
		to.components = from.components;
		to.overrides = from.overrides;
	}

	std::string node_key(std::size_t world, std::uint64_t id)
	{
		return std::format("node:{}:{:016x}", world, id);
	}

	// The undo record of one commit: the nodes it changed, keyed node:<world>:<id> for conflicts. A node that only
	// moved among its siblings has a key too: undoing the change puts it back, over a later reorder of someone else.
	class scene_change final : public edt::history_change
	{
	public:
		scene_change(std::vector<scn::world_desc>& worlds, edt::scene_history_index& index, std::vector<edt::scene_node_change> changes)
			: m_worlds(worlds), m_index(index), m_changes(std::move(changes))
		{
			for (const auto& change : m_changes) {
				m_keys.push_back(node_key(change.world, change.id));
			}
		}

		void undo() override { edt::apply_scene_changes(m_worlds, m_index, m_changes, true); }
		void redo() override { edt::apply_scene_changes(m_worlds, m_index, m_changes, false); }
		const std::vector<std::string>& keys() const override { return m_keys; }

		// Undo removes the nodes the commit created, with everything under them: a node someone else put there
		// since would go with them. The nodes of this commit are not in the way: undo puts them back first.
		std::optional<std::string> undo_conflict() const override
		{
			std::set<std::pair<std::size_t, std::uint64_t>> own;
			std::vector<std::pair<std::size_t, std::uint64_t>> created;
			for (const auto& change : m_changes) {
				own.insert({ change.world, change.id });
				if (!change.before) {
					created.push_back({ change.world, change.id });
				}
			}
			for (const auto& [world, id] : created) {
				if (world >= m_worlds.size()) {
					continue;
				}
				const prefab_node* node = find_node(std::as_const(m_worlds[world]).get_root(), id);
				if (!node) {
					continue;
				}
				std::optional<std::string> foreign;
				const auto walk = [&](const auto& self, const prefab_node& n) -> void {
					for (const auto& child : n.children) {
						if (foreign) {
							return;
						}
						if (!own.contains({ world, child.node_id })) {
							foreign = node_key(world, child.node_id);
							return;
						}
						self(self, child);
					}
				};
				walk(walk, *node);
				if (foreign) {
					return foreign;
				}
			}
			return std::nullopt;
		}

	private:
		std::vector<scn::world_desc>& m_worlds;
		edt::scene_history_index& m_index;
		std::vector<edt::scene_node_change> m_changes;
		std::vector<std::string> m_keys;
	};

}

scn::prefab_desc::prefab_node edt::shallow_copy(const scn::prefab_desc::prefab_node& node)
{
	prefab_node copy;
	assign_shallow(copy, node);
	return copy;
}

bool edt::same_shallow(const scn::prefab_desc::prefab_node& a, const scn::prefab_desc::prefab_node& b)
{
	if (a.name != b.name || a.node_id != b.node_id || a.position != b.position || a.rotation != b.rotation || a.scale != b.scale
		|| a.has_transform != b.has_transform || a.components.size() != b.components.size() || !same_records(a.overrides, b.overrides)) {
		return false;
	}
	for (const auto& [key, component] : a.components) {
		const auto other = b.components.find(key);
		// The parent desc by reference: what it points to is a resource, not part of the node.
		if (other == b.components.end() || component.type_name != other->second.type_name
			|| !(component.parent_desc.get_tag() == other->second.parent_desc.get_tag()) || component.overrides != other->second.overrides) {
			return false;
		}
	}
	return true;
}

edt::scene_node_state edt::placement_of(const scene_node_state& state, std::uint64_t id)
{
	scene_node_state out{ state.parent, state.previous, state.depth, {} };
	out.node.node_id = id != 0 ? id : state.node.node_id;
	return out;
}

bool edt::scene_history_index::rebuild(const std::vector<scn::world_desc>& worlds)
{
	m_worlds.assign(worlds.size(), {});
	m_valid = false;
	for (std::size_t w = 0; w < worlds.size(); ++w) {
		auto& index = m_worlds[w];
		bool ok = true;
		const auto walk = [&](const auto& self, const prefab_node& node, std::uint64_t parent, std::uint64_t previous, int depth) -> void {
			if (!ok) {
				return;
			}
			// The root is found by its place, a default world's root has no id.
			if ((depth > 0 && node.node_id == 0) || index.contains(node.node_id)) {
				ok = false;
				return;
			}
			index.emplace(node.node_id, scene_node_state{ parent, previous, depth, shallow_copy(node) });
			std::uint64_t before = 0;
			for (const auto& child : node.children) {
				self(self, child, node.node_id, before, depth + 1);
				before = child.node_id;
			}
		};
		walk(walk, worlds[w].get_root(), 0, 0, 0);
		if (!ok) {
			m_worlds.clear();
			return false;
		}
	}
	m_valid = true;
	return true;
}

edt::scene_history_index::diff_result edt::scene_history_index::diff(std::vector<scn::world_desc>& worlds,
	std::unique_ptr<history_change>& change, scene_commit_stats& stats)
{
	stats = {};
	if (!m_valid || worlds.size() != m_worlds.size()) {
		return diff_result::unavailable;
	}
	std::vector<scene_node_change> changes;
	std::vector<std::unordered_set<std::uint64_t>> seen(worlds.size());
	for (std::size_t w = 0; w < worlds.size(); ++w) {
		auto& index = m_worlds[w];
		bool ok = true;
		const auto walk = [&](const auto& self, const prefab_node& node, std::uint64_t parent, std::uint64_t previous, int depth) -> void {
			if (!ok) {
				return;
			}
			if ((depth > 0 && node.node_id == 0) || !seen[w].insert(node.node_id).second) {
				ok = false;
				return;
			}
			++stats.nodes_compared;
			const auto it = index.find(node.node_id);
			if (it == index.end()) {
				++stats.snapshots_copied;
				changes.push_back({ w, node.node_id, std::nullopt, scene_node_state{ parent, previous, depth, shallow_copy(node) }, false });
			} else {
				scene_node_state& old = it->second;
				const bool same_node = same_shallow(old.node, node);
				if (!same_node) {
					++stats.snapshots_copied;
					changes.push_back({ w, node.node_id, old, scene_node_state{ parent, previous, depth, shallow_copy(node) }, false });
				} else if (old.parent != parent || old.previous != previous) {
					// Moved, not edited: only where it sits is kept, so applying it never touches what is in the node.
					changes.push_back({ w, node.node_id, placement_of(old), placement_of({ parent, previous, depth, {} }, node.node_id), true });
				} else {
					// Under a moved parent a node keeps its place among its siblings but not its depth.
					old.depth = depth;
				}
			}
			std::uint64_t before = 0;
			for (const auto& child : node.children) {
				self(self, child, node.node_id, before, depth + 1);
				before = child.node_id;
			}
		};
		walk(walk, worlds[w].get_root(), 0, 0, 0);
		if (!ok) {
			return diff_result::unavailable;
		}
	}
	// Removed nodes, parents first; their index entries go below, so the states are moved out.
	std::vector<scene_node_change> removed;
	for (std::size_t w = 0; w < worlds.size(); ++w) {
		for (auto& [id, state] : m_worlds[w]) {
			if (!seen[w].contains(id)) {
				removed.push_back({ w, id, std::move(state), std::nullopt, false });
			}
		}
	}
	std::stable_sort(removed.begin(), removed.end(), [](const scene_node_change& a, const scene_node_change& b) {
		return a.before->depth < b.before->depth;
	});
	changes.insert(changes.end(), std::make_move_iterator(removed.begin()), std::make_move_iterator(removed.end()));
	stats.changes = changes.size();
	if (changes.empty()) {
		return diff_result::unchanged;
	}
	for (const auto& entry : changes) {
		if (entry.positional) {
			set_place(entry.world, entry.id, *entry.after);
		} else {
			set(entry.world, entry.id, entry.after);
		}
	}
	change = std::make_unique<scene_change>(worlds, *this, std::move(changes));
	return diff_result::changed;
}

void edt::scene_history_index::refresh_depths(std::size_t world, const scn::world_desc& desc)
{
	if (world >= m_worlds.size()) {
		return;
	}
	auto& index = m_worlds[world];
	const auto walk = [&](const auto& self, const prefab_node& node, int depth) -> void {
		if (const auto it = index.find(node.node_id); it != index.end()) {
			it->second.depth = depth;
		}
		for (const auto& child : node.children) {
			self(self, child, depth + 1);
		}
	};
	walk(walk, desc.get_root(), 0);
}

void edt::scene_history_index::set_place(std::size_t world, std::uint64_t id, const scene_node_state& place)
{
	if (world >= m_worlds.size()) {
		return;
	}
	if (const auto it = m_worlds[world].find(id); it != m_worlds[world].end()) {
		it->second.parent = place.parent;
		it->second.previous = place.previous;
		it->second.depth = place.depth;
	}
}

std::vector<std::size_t> edt::scene_history_index::take_touched()
{
	return std::exchange(m_touched, {});
}

void edt::scene_history_index::touch(std::size_t world)
{
	if (std::find(m_touched.begin(), m_touched.end(), world) == m_touched.end()) {
		m_touched.push_back(world);
	}
}

void edt::scene_history_index::set(std::size_t world, std::uint64_t id, const std::optional<scene_node_state>& state)
{
	if (world >= m_worlds.size()) {
		return;
	}
	if (state) {
		m_worlds[world][id] = *state;
	} else {
		m_worlds[world].erase(id);
	}
}

void edt::apply_scene_changes(std::vector<scn::world_desc>& worlds, scene_history_index& index,
	const std::vector<scene_node_change>& changes, bool undo)
{
	const auto target = [undo](const scene_node_change& change) -> const std::optional<scene_node_state>& {
		return undo ? change.before : change.after;
	};
	// Nodes that stay, parents before their children, so a node placed under one that comes back finds it.
	std::vector<const scene_node_change*> placed;
	for (const auto& change : changes) {
		if (change.world < worlds.size() && target(change)) {
			placed.push_back(&change);
		}
	}
	std::stable_sort(placed.begin(), placed.end(), [&](const scene_node_change* a, const scene_node_change* b) {
		return target(*a)->depth < target(*b)->depth;
	});
	// Within one depth a node goes right after its previous sibling, so that one has to be in place first.
	using node_key = std::pair<std::size_t, std::uint64_t>;
	std::vector<const scene_node_change*> ordered;
	ordered.reserve(placed.size());
	for (std::size_t begin = 0; begin < placed.size();) {
		std::size_t end = begin;
		while (end < placed.size() && target(*placed[end])->depth == target(*placed[begin])->depth) {
			++end;
		}
		std::set<node_key> in_depth;
		std::map<node_key, const scene_node_change*> by_previous;
		for (std::size_t i = begin; i < end; ++i) {
			in_depth.insert({ placed[i]->world, placed[i]->id });
			if (target(*placed[i])->previous != 0) {
				by_previous[{ placed[i]->world, target(*placed[i])->previous }] = placed[i];
			}
		}
		for (std::size_t i = begin; i < end; ++i) {
			const std::uint64_t previous = target(*placed[i])->previous;
			if (previous != 0 && in_depth.contains({ placed[i]->world, previous })) {
				continue; // placed after its previous sibling, below
			}
			for (const scene_node_change* next = placed[i]; next;) {
				ordered.push_back(next);
				const auto it = by_previous.find({ next->world, next->id });
				next = it == by_previous.end() ? nullptr : it->second;
			}
		}
		begin = end;
	}
	for (const auto* change : ordered) {
		const scene_node_state& state = *target(*change);
		prefab_node& root = worlds[change->world].get_root();
		index.touch(change->world);
		if (change->positional) {
			node_place place;
			// Gone (someone deleted it since): nothing to put back.
			if (state.depth > 0 && find_place(root.children, change->id, place)) {
				prefab_node* parent = find_node(root, state.parent);
				const std::uint64_t previous = place.index == 0 ? 0 : (*place.siblings)[place.index - 1].node_id;
				if (!parent || &parent->children != place.siblings || previous != state.previous) {
					prefab_node moved = std::move((*place.siblings)[place.index]);
					place.siblings->erase(place.siblings->begin() + static_cast<std::ptrdiff_t>(place.index));
					insert_at(root, std::move(moved), state);
				}
				index.set_place(change->world, change->id, state);
			}
			continue;
		}
		if (state.depth == 0) {
			assign_shallow(root, state.node);
		} else {
			node_place place;
			if (find_place(root.children, change->id, place)) {
				prefab_node* parent = find_node(root, state.parent);
				const auto& siblings = *place.siblings;
				const bool same_parent = parent && &parent->children == place.siblings;
				const std::uint64_t previous = place.index == 0 ? 0 : siblings[place.index - 1].node_id;
				if (!same_parent || previous != state.previous) {
					prefab_node moved = std::move((*place.siblings)[place.index]);
					place.siblings->erase(place.siblings->begin() + static_cast<std::ptrdiff_t>(place.index));
					assign_shallow(moved, state.node);
					insert_at(root, std::move(moved), state);
				} else {
					assign_shallow((*place.siblings)[place.index], state.node);
				}
			} else {
				insert_at(root, shallow_copy(state.node), state);
			}
		}
		index.set(change->world, change->id, state);
	}
	// Then the nodes that go, with what is left under them (their descendants that stay were placed elsewhere above).
	for (const auto& change : changes) {
		if (change.world >= worlds.size() || target(change)) {
			continue;
		}
		index.touch(change.world);
		node_place place;
		if (find_place(worlds[change.world].get_root().children, change.id, place)) {
			place.siblings->erase(place.siblings->begin() + static_cast<std::ptrdiff_t>(place.index));
		}
		index.set(change.world, change.id, std::nullopt);
	}
	// The nodes below a moved one were not part of the change, but their depth is: a later change records it as its
	// `before`, and undo places by depth.
	std::vector<bool> touched(worlds.size(), false);
	for (const auto& change : changes) {
		if (change.world < worlds.size()) {
			touched[change.world] = true;
		}
	}
	for (std::size_t w = 0; w < worlds.size(); ++w) {
		if (touched[w]) {
			index.refresh_depths(w, worlds[w]);
		}
	}
}
