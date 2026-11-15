#pragma once
#include <snk/editor/edt_document.h>
#include <cstdint>
#include <vector>

namespace edt
{
	class prefab_document;
	class scene_document;

	// Whose selection the Inspector shows: the level's (the default) or one node of one prefab document's working copy.
	// One field instead of swapped callbacks: the level and the prefab window never both own the Inspector.
	class inspector_target
	{
	public:
		bool is_prefab() const { return m_prefab != document_id{}; }
		document_id prefab() const { return m_prefab; }
		const std::vector<std::uint64_t>& path() const { return m_path; }
		// How often the document replaced its working copy when the node pointer was taken: a pointer taken before is stale.
		std::uint64_t replaced() const { return m_replaced; }

		void select_prefab(document_id prefab, std::vector<std::uint64_t> path, std::uint64_t replaced)
		{
			m_prefab = prefab;
			m_path = std::move(path);
			m_replaced = replaced;
		}
		void set_replaced(std::uint64_t replaced) { m_replaced = replaced; }
		// The level owns the Inspector again.
		void reset()
		{
			m_prefab = document_id{};
			m_path.clear();
			m_replaced = 0;
		}

	private:
		document_id m_prefab{};
		std::vector<std::uint64_t> m_path;
		std::uint64_t m_replaced = 0;
	};

	// Where an Inspector edit goes: to the prefab document the selection belongs to (never to the level), else the level's
	// commit. False: the edit had nowhere to go (the prefab is gone).
	bool route_node_changed(const inspector_target& target, prefab_document* open_prefab, scene_document& level);
}
