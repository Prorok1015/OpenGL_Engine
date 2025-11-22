#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <algorithm>
#include <span>

namespace ds
{
	template<class T, class KEY, class SUBKEY>
	struct rtree
	{
		static constexpr auto MAX_ENTRIES = 3u;
		static constexpr auto MIN_ENTRIES = MAX_ENTRIES / 2;

		struct entry
		{
			KEY box;
			T val;
			uint32_t child = 0;
		};

		struct node
		{
			KEY box;
			std::vector<entry> entries;
			bool is_leaf = true;
			uint32_t parent = 0;

			static node create_leaf() { return node{}; }
			static node create_branch() { return node{ .is_leaf = false }; }
		};

		std::vector<node> data;
		uint32_t root = 0;

		void build(std::vector<std::pair<KEY, T>> items);
		std::vector<T> query(const SUBKEY& box) const;
		std::vector<T> query(const KEY& box) const;

		void query_recursive(uint32_t nodeIndex, const KEY& box, std::vector<T>& results) const;
		void query_recursive(uint32_t nodeIndex, const SUBKEY& box, std::vector<T>& results) const;
	};

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