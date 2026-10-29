#pragma once
#include <snk/editor/edt_document_history.h>
#include <snk/scene/level/scn_world_desc.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The scene's side of the undo history (#141-#144): the committed state of every world by node id, and the change one
// commit made, found by comparing the worlds with it.
namespace edt
{
	// A node as the history keeps it: its own fields without its children, and where it sits.
	struct scene_node_state
	{
		std::uint64_t parent = 0;   // the id of its parent; nothing for the root
		std::uint64_t previous = 0; // the sibling just before it; 0: the first child
		int depth = 0;              // 0: the world's root, found by its place (a default root has no id)
		scn::prefab_desc::prefab_node node; // children left out
	};

	// What the last commit cost: every node is compared, only the changed ones are copied.
	struct scene_commit_stats
	{
		std::size_t nodes_compared = 0;
		std::size_t snapshots_copied = 0;
		std::size_t changes = 0;
	};

	// One node of one world before and after a commit; no `before`: it was created, no `after`: it was removed.
	struct scene_node_change
	{
		std::size_t world = 0;
		std::uint64_t id = 0;
		std::optional<scene_node_state> before;
		std::optional<scene_node_state> after;
		// Only its place among unchanged siblings moved (a sibling before it came or went): not an edit of the node.
		bool positional = false;
	};

	class scene_history_index
	{
	public:
		enum class diff_result
		{
			changed,
			unchanged,
			// A node without an id, an id twice in one world, or another number of worlds: nothing can be compared.
			unavailable,
		};

		// From the worlds as they are now; false (and not valid) when a world has a node without an id or an id twice.
		bool rebuild(const std::vector<scn::world_desc>& worlds);
		bool valid() const { return m_valid; }

		// What changed since the index was taken, as one history change applying to `worlds`; the index moves on to now.
		diff_result diff(std::vector<scn::world_desc>& worlds, std::unique_ptr<history_change>& change, scene_commit_stats& stats);

		// Brings the node `id` of `world` to `state` in the index (nullopt: removes it); used when a change is applied.
		void set(std::size_t world, std::uint64_t id, const std::optional<scene_node_state>& state);
		// Takes the depth of every node of `world` from `desc`, whose structure the index already holds.
		void refresh_depths(std::size_t world, const scn::world_desc& desc);

	private:
		std::vector<std::unordered_map<std::uint64_t, scene_node_state>> m_worlds;
		bool m_valid = false;
	};

	// Copies a node's own fields, not its children.
	scn::prefab_desc::prefab_node shallow_copy(const scn::prefab_desc::prefab_node& node);
	// Whether two nodes are equal in their own fields (components by type, parent reference and overrides).
	bool same_shallow(const scn::prefab_desc::prefab_node& a, const scn::prefab_desc::prefab_node& b);

	// Brings the nodes of `changes` in `worlds` to their state before (`undo`) or after the commit, and the index with
	// them: nodes placed parents first, removed ones last.
	void apply_scene_changes(std::vector<scn::world_desc>& worlds, scene_history_index& index,
		const std::vector<scene_node_change>& changes, bool undo);
}
