#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <algorithm>
#include <span>

namespace ds
{
	/*
		needs to implemet functions for KEY type:
		 - void expand(KEY& box, const KEY& other);
		 - bool intersects(const KEY& a, const KEY& b);
		 - double area(const KEY& box);
		 - point2d center(const KEY& box);

		 needs to implement functions for SUBKEY type:
		 - bool contains(const KEY& a, const SUBKEY& b);
	*/
	template<class T, class KEY, class SUBKEY>
	struct rtree
	{
		static constexpr auto MAX_ENTRIES = 3u;
		static constexpr auto MIN_ENTRIES = MAX_ENTRIES / 2;

		static constexpr auto INVALID_NODE = std::numeric_limits<uint32_t>::max();

		struct entry
		{
			KEY box;
			T val;
			uint32_t child = INVALID_NODE;
		};

		struct node
		{
			KEY box;
			std::vector<entry> entries;
			bool is_leaf = true;
			uint32_t parent = INVALID_NODE;

			static node create_leaf() { return node{}; }
			static node create_branch() { return node{ .is_leaf = false }; }
		};

		std::vector<node> data;
		uint32_t root = 0;

		void insert(const KEY& box, const T& val);
		void insert_recursive(uint32_t node_idx, const KEY& box, const T& val);
		uint32_t choose_subtree(const KEY& box, uint32_t start_idx) const;
		bool split_node(uint32_t node_idx, uint32_t& new_idx);
		void adjust_tree(uint32_t node_idx, uint32_t new_node_idx);

		void split_impl(uint32_t node_idx, uint32_t& new_idx);

		void build(std::vector<std::pair<KEY, T>> items);
		std::vector<T> query(const SUBKEY& box) const;
		std::vector<T> query(const KEY& box) const;

		void query_recursive(uint32_t nodeIndex, const KEY& box, std::vector<T>& results) const;
		void query_recursive(uint32_t nodeIndex, const SUBKEY& box, std::vector<T>& results) const;
	};

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::insert(const KEY& box, const T& val)
	{
		if (data.empty()) {
			node rootNode = node::create_leaf();
			rootNode.box = box;
			rootNode.entries.push_back(entry{ box, val });
			data.push_back(std::move(rootNode));
			root = 0;
			return;
		}

		insert_recursive(root, box, val);
	}

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::insert_recursive(uint32_t node_idx, const KEY& box, const T& val)
	{
		auto& node = data[node_idx];
		if (node.is_leaf) {
			node.entries.push_back(entry{ box, val });
			expand(node.box, box);
			uint32_t newNodeIdx = INVALID_NODE;
			split_node(node_idx, newNodeIdx);
			adjust_tree(node_idx, newNodeIdx);
		} else {
			uint32_t childIdx = choose_subtree(box, node_idx);
			insert_recursive(childIdx, box, val);
		}
	}

	template<class T, class KEY, class SUBKEY>
	uint32_t rtree<T, KEY, SUBKEY>::choose_subtree(const KEY& box, uint32_t start_idx) const
	{
		const auto& node = data[start_idx];
		uint32_t bestIdx = 0;
		double bestEnlargement = std::numeric_limits<double>::max();
		double bestArea = std::numeric_limits<double>::max();
		for (const auto& entry : node.entries) {
			KEY enlargedBox = entry.box;
			expand(enlargedBox, box);
			const double boxarea = area(entry.box);
			double enlargement = area(enlargedBox) - boxarea;
			if (enlargement < bestEnlargement ||
				(enlargement == bestEnlargement && boxarea < bestArea)) {
				bestEnlargement = enlargement;
				bestArea = boxarea;
				bestIdx = entry.child;
			}
		}

		ASSERT_MSG(bestIdx != 0, "choose_subtree found invalid child");

		return bestIdx;
	}

	template<class T, class KEY, class SUBKEY>
	bool rtree<T, KEY, SUBKEY>::split_node(uint32_t node_idx, uint32_t& new_idx)
	{
		auto& node = data[node_idx];
		if (node.entries.size() <= MAX_ENTRIES) {
			new_idx = INVALID_NODE;
			return false;
		}

		split_impl(node_idx, new_idx);
		return true;
	}

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::split_impl(uint32_t node_idx, uint32_t& new_idx)
	{
		// quadratic split algorithm
		auto& oldnode = data[node_idx];

		auto entries = std::move(oldnode.entries);
		oldnode.entries.clear();

		uint32_t seed1 = INVALID_NODE;
		uint32_t seed2 = INVALID_NODE;

		double maxWastedArea = -1.0;
		for (size_t i = 0; i < entries.size(); ++i) {
			for (size_t j = i + 1; j < entries.size(); ++j) {
				KEY mergedBox = entries[i].box;
				expand(mergedBox, entries[j].box);

				float wastedArea = area(mergedBox) - area(entries[i].box) - area(entries[j].box);

				if (wastedArea > maxWastedArea) {
					maxWastedArea = wastedArea;
					seed1 = i;
					seed2 = j;
				}
			}
		}


	}

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::adjust_tree(uint32_t node_idx, uint32_t new_node_idx)
	{
		if (node_idx == root) {
			if (new_node_idx != INVALID_NODE) {
				// create new root
				node newRoot = node::create_branch();
				newRoot.entries.push_back(entry{ data[node_idx].box, {}, node_idx });
				expand(newRoot.box, data[node_idx].box);
				newRoot.entries.push_back(entry{ data[new_node_idx].box, {}, new_node_idx });
				expand(newRoot.box, data[new_node_idx].box);
				data.push_back(std::move(newRoot));
				root = uint32_t(data.size() - 1);
				data[node_idx].parent = root;
				data[new_node_idx].parent = root;
			}
			return;
		}

		uint32_t parentIdx = data[node_idx].parent;
		auto& parentNode = data[parentIdx];
		// update existing entry
		for (auto& entry : parentNode.entries) {
			if (entry.child == node_idx) {
				entry.box = data[node_idx].box; // probobly useless
				break;
			}
		}

		expand(parentNode.box, data[node_idx].box);
		if (new_node_idx != INVALID_NODE) {
			parentNode.entries.push_back(entry{ data[new_node_idx].box, {}, new_node_idx });
			expand(parentNode.box, data[new_node_idx].box);
		}

		uint32_t newParentIdx = INVALID_NODE;
		split_node(parentIdx, newParentIdx);
		adjust_tree(parentIdx, newParentIdx);
	}

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::build(std::vector<std::pair<KEY, T>> items)
	{
		struct localentry
		{
			uint32_t parent;
			T val;
			KEY box;
		};

		data.clear();
		root = 0;

		std::vector<localentry> entries;
		entries.reserve(items.size());
		uint32_t index = 0;
		for (const auto& [box, val] : items) {
			entries.push_back(localentry{ index++, val, box });
		}

		auto performSTR = [this](std::vector<localentry>& currentEntries, bool creatingLeaves) {
			std::vector<localentry> parentEntries;

			size_t N = currentEntries.size();
			size_t P = (N + MAX_ENTRIES - 1) / MAX_ENTRIES;
			size_t S = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(P))));

			// sort X
			std::sort(currentEntries.begin(), currentEntries.end(), [](const auto& a, const auto& b) {
				return center(a.box).x < center(b.box).x;
			});

			size_t sliceSize = S * MAX_ENTRIES;

			for (size_t i = 0; i < N; i += sliceSize) {
				auto sliceEnd = std::min(i + sliceSize, N);
				std::span<localentry> slice{ currentEntries.data() + i, sliceEnd - i };

				// sort slice Y
				std::sort(slice.begin(), slice.end(), [](const auto& a, const auto& b) {
					return center(a.box).y < center(b.box).y;
				});

				// packing
				for (size_t j = 0; j < slice.size(); j += MAX_ENTRIES) {
					const size_t end = std::min(j + MAX_ENTRIES, slice.size());
					auto pack = slice.subspan(j, end - j);

					node newNode = node::create_branch();

					if (creatingLeaves) {
						node leaf = node::create_leaf();
						leaf.parent = 0;
						leaf.entries.reserve(pack.size());
						for (const auto& e : pack) {
							leaf.entries.push_back(entry{e.box, e.val});
							expand(leaf.box, e.box);
						}
						newNode = std::move(leaf);
					} else {
						auto& branch = newNode;
						branch.entries.reserve(pack.size());
						for (const auto& e : pack) {
							branch.entries.push_back(entry{ e.box, {}, e.parent });
							expand(branch.box, e.box);
						}
						newNode = std::move(branch);
					}

					data.push_back(std::move(newNode));
					uint32_t newNodeIndex = uint32_t(data.size() - 1);

					parentEntries.push_back(localentry{ newNodeIndex, {}, data.back().box });
				}
			}
			return parentEntries;
		};


		// level 0
		auto currentLevelNodes = performSTR(entries, true);

		// level 1+
		while (currentLevelNodes.size() > 1) {
			currentLevelNodes = performSTR(currentLevelNodes, false);
		}

		if (!currentLevelNodes.empty()) {
			root = currentLevelNodes[0].parent;
		}
	}

	template<class T, class KEY, class SUBKEY>
	std::vector<T> rtree<T, KEY, SUBKEY>::query(const SUBKEY& point) const
	{
		if (data.empty()) return {};

		std::vector<T> results;
		query_recursive(root, point, results);
		return results;
	}

	template<class T, class KEY, class SUBKEY>
	std::vector<T> rtree<T, KEY, SUBKEY>::query(const KEY& box) const
	{
		if (data.empty()) return {};

		std::vector<T> results;
		query_recursive(root, box, results);
		return results;
	}

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::query_recursive(uint32_t index, const KEY& area, std::vector<T>& result) const
	{
		const auto& node = data[index];

		if (!intersects(node.box, area)) return;

		if (node.is_leaf) {
			for (const auto& leaf : node.entries) {
				if (!intersects(leaf.box, area)) continue;

				result.push_back(leaf.val);
			}
		} else {
			for (const auto& child : node.entries) {
				query_recursive(child.child, area, result);
			}
		}
	}

	template<class T, class KEY, class SUBKEY>
	void rtree<T, KEY, SUBKEY>::query_recursive(uint32_t index, const SUBKEY& area, std::vector<T>& result) const
	{
		const auto& node = data[index];

		if (!contains(node.box, area)) return;

		if (node.is_leaf) {
			for (const auto& leaf : node.entries) {
				if (!contains(leaf.box, area)) continue;

				result.push_back(leaf.val);
			}
		} else {
			for (const auto& ent : node.entries) {
				query_recursive(ent.child, area, result);
			}
		}
	}
}