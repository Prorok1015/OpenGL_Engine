#pragma once
#include <algorithm>
#include <span>

template<class T, class KEY, class POLICY, class STORAGE>
void ds::rtree<T, KEY, POLICY, STORAGE>::build(std::vector<std::pair<KEY, T>> items)
{
	data.clear();
	root = 0;

	std::vector<entry> entries;
	entries.reserve(items.size());
	for (const auto& [box, value] : items) {
		entries.push_back(entry::create_with_value(box, value));
	}

	auto performSTR = [this](std::vector<entry>& currentEntries, bool creatingLeaves) {
		std::vector<entry> parentEntries;

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
			std::span<entry> slice{ currentEntries.data() + i, sliceEnd - i };

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
					for (const auto& e : pack) {
						leaf.add(e);
						expand(leaf.box, e.box);
					}
					newNode = std::move(leaf);
				} else {
					auto& branch = newNode;
					for (const auto& e : pack) {
						branch.add(e);
						expand(branch.box, e.box);
					}
					newNode = std::move(branch);
				}

				parentEntries.push_back(entry::create_with_child(newNode.box, index_type(data.size()) ));

				data.push_back(std::move(newNode));
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
		root = currentLevelNodes[0].get_child();
	}
}



template<class T, class KEY, class POLICY, class STORAGE>
void ds::rtree<T, KEY, POLICY, STORAGE>::insert(const KEY& box, const T& val)
{
	if (data.empty()) {
		node rootNode = node::create_leaf();
		rootNode.box = box;
		rootNode.add_as_leaf(box, val);
		data.push_back(std::move(rootNode));
		root = 0;
		return;
	}

	insert_recursive(root, box, val);
}

template<class T, class KEY, class POLICY, class STORAGE>
void ds::rtree<T, KEY, POLICY, STORAGE>::insert_recursive(index_type node_idx, const KEY& box, const T& val)
{
	auto& node = data[node_idx];
	if (node.is_leaf()) {
		node.add_as_leaf(box, val);
		expand(node.box, box);
		auto newNodeIdx = node::INVALID_NODE;
		split_node(node_idx, newNodeIdx);
		adjust_tree(node_idx, newNodeIdx);
	}
	else {
		auto childIdx = choose_subtree(box, node_idx);
		insert_recursive(childIdx, box, val);
	}
}

template<class T, class KEY, class POLICY, class STORAGE>
ds::rtree<T, KEY, POLICY, STORAGE>::index_type ds::rtree<T, KEY, POLICY, STORAGE>::choose_subtree(const KEY& box, index_type start_idx) const
{
	const auto& node = data[start_idx];
	auto bestIdx = node::INVALID_NODE;
	double bestEnlargement = std::numeric_limits<double>::max();
	double bestArea = std::numeric_limits<double>::max();
	for (const auto& entry : node.entries) {
		auto enlargedBox = entry.box;
		expand(enlargedBox, box);
		const double boxarea = area(entry.box);
		double enlargement = area(enlargedBox) - boxarea;
		if (enlargement < bestEnlargement ||
			(enlargement == bestEnlargement && boxarea < bestArea)) {
			bestEnlargement = enlargement;
			bestArea = boxarea;
			bestIdx = entry.get_child();
		}
	}

	ASSERT_MSG(bestIdx != node::INVALID_NODE, "choose_subtree found invalid child");

	return bestIdx;
}

template<class T, class KEY, class POLICY, class STORAGE>
bool ds::rtree<T, KEY, POLICY, STORAGE>::split_node(index_type node_idx, index_type& new_idx)
{
	auto& node = data[node_idx];
	if (node.size() <= MAX_ENTRIES) {
		new_idx = node::INVALID_NODE;
		return false;
	}

	new_idx = POLICY::split(data, node_idx, root);
	return true;
}

template<class T, class KEY, class POLICY, class STORAGE>
void ds::rtree<T, KEY, POLICY, STORAGE>::adjust_tree(index_type node_idx, index_type new_node_idx)
{
	if (node_idx == root) {
		return;
	}

	auto parent_idx = data[node_idx].parent;

	for (auto& entry : data[parent_idx].entries) {
		if (entry.get_child() == node_idx) {
			entry.box = data[node_idx].box;
			break;
		}
	}

	auto newParentIdx = node::INVALID_NODE;
	if (split_node(parent_idx, newParentIdx)) {
		adjust_tree(parent_idx, newParentIdx);
	} else {
		if (new_node_idx != node::INVALID_NODE) {
			KEY box{};
			for (auto& entry : data[parent_idx].entries) {
				expand(box, entry.box);
			}
			data[parent_idx].box = box;
		}
		adjust_tree(parent_idx);
	}
}
//
//template<class T, class KEY, class POLICY>
//std::vector<T> ds::rtree<T, KEY, POLICY>::query(const KEY& point, std::function<void(const node&, index_type)> func) const
//{
//	if (data.empty()) return {};
//
//	std::vector<T> results;
//	query_recursive(root, point, results, func, 0);
//	return results;
//}

template<class T, class KEY, class POLICY, class STORAGE>
std::vector<T> ds::rtree<T, KEY, POLICY, STORAGE>::query(const KEY& box) const
{
	if (data.empty()) return {};

	std::vector<T> results;
	query_recursive(root, box, results);
	return results;
}

template<class T, class KEY, class POLICY, class STORAGE>
void ds::rtree<T, KEY, POLICY, STORAGE>::query_recursive(index_type index, const KEY& area, std::vector<T>& result) const
{
	const auto& node = data[index];

	if (!intersects(node.box, area)) return;

	if (node.is_leaf()) {
		for (const auto& leaf : node.entries) {
			if (!intersects(leaf.box, area)) continue;

			result.push_back(leaf.get_value());
		}
	} else {
		for (const auto& child : node.entries) {
			if (!intersects(child.box, area)) continue;

			query_recursive(child.get_child(), area, result);
		}
	}
}
//
//template<class T, class KEY, class POLICY>
//void ds::rtree<T, KEY, POLICY>::query_recursive(index_type index, const KEY& area, std::vector<T>& result, const std::function<void(const node&, index_type)>& func, index_type depth) const
//{
//	const auto& node = data[index];
//
//	if (!intersects(node.box, area)) return;
//
//	func(node, depth++);
//
//	if (node::is_leaf(node)) {
//		for (const auto& leaf : node.entries) {
//			if (!intersects(leaf.box, area)) continue;
//
//			result.push_back(leaf.get_value());
//		}
//	}
//	else {
//		for (const auto& ent : node.entries) {
//			query_recursive(ent.get_child(), area, result, func, depth);
//		}
//	}
//}