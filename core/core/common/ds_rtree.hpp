#pragma once
#include <algorithm>
#include <span>

template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::build(std::vector<std::pair<KEY, T>> items)
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
						leaf.entries.push_back(entry{ e.box, e.val });
						expand(leaf.box, e.box);
					}
					newNode = std::move(leaf);
				}
				else {
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



template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::insert(const KEY& box, const T& val)
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

template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::insert_recursive(uint32_t node_idx, const KEY& box, const T& val)
{
	auto& node = data[node_idx];
	if (node::is_leaf(node)) {
		node.entries.push_back(entry{ box, val });
		expand(node.box, box);
		uint32_t newNodeIdx = node::INVALID_NODE;
		split_node(node_idx, newNodeIdx);
		adjust_tree(node_idx, newNodeIdx);
	}
	else {
		uint32_t childIdx = choose_subtree(box, node_idx);
		insert_recursive(childIdx, box, val);
	}
}

template<class T, class KEY, class POLICY>
uint32_t ds::rtree<T, KEY, POLICY>::choose_subtree(const KEY& box, uint32_t start_idx) const
{
	const auto& node = data[start_idx];
	uint32_t bestIdx = node::INVALID_NODE;
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
			bestIdx = entry.child;
		}
	}

	ASSERT_MSG(bestIdx != node::INVALID_NODE, "choose_subtree found invalid child");

	return bestIdx;
}

template<class T, class KEY, class POLICY>
bool ds::rtree<T, KEY, POLICY>::split_node(uint32_t node_idx, uint32_t& new_idx)
{
	auto& node = data[node_idx];
	if (node.entries.size() <= MAX_ENTRIES) {
		new_idx = node::INVALID_NODE;
		return false;
	}

	new_idx = POLICY::split(data, node_idx, root);
	//split_impl(node_idx, new_idx);
	return true;
}

template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::split_impl(uint32_t node_idx, uint32_t& new_idx)
{
	// quadratic split algorithm
	const auto& oldnode = data[node_idx];
	const auto& entries = oldnode.entries;

	uint32_t seed1 = node::INVALID_NODE;
	uint32_t seed2 = node::INVALID_NODE;

	double maxWastedArea = std::numeric_limits<double>::lowest();
	for (size_t i = 0; i < entries.size(); ++i) {
		for (size_t j = i + 1; j < entries.size(); ++j) {
			auto mergedBox = entries[i].box;
			expand(mergedBox, entries[j].box);

			double wastedArea = area(mergedBox) - area(entries[i].box) - area(entries[j].box);

			if (wastedArea > maxWastedArea) {
				maxWastedArea = wastedArea;
				seed1 = i;
				seed2 = j;
			}
		}
	}

	ASSERT_MSG(seed1 != node::INVALID_NODE && seed2 != node::INVALID_NODE, "seeds incorrect!");

	node left = node::is_leaf(oldnode) ? node::create_leaf() : node::create_branch();
	node right = node::is_leaf(oldnode) ? node::create_leaf() : node::create_branch();
	left.parent = oldnode.parent;
	right.parent = oldnode.parent;

	left.box = entries[seed1].box;
	right.box = entries[seed2].box;

	left.entries.push_back(entries[seed1]);
	right.entries.push_back(entries[seed2]);

	for (size_t i = 0; i < entries.size(); ++i)
	{
		if (i == seed1 || i == seed2) continue;

		const auto& ent = entries[i];
		auto box1 = left.box;
		auto box2 = right.box;

		expand(box1, ent.box);
		expand(box2, ent.box);

		const double leftarea = area(left.box);
		const double extboxarea1 = area(box1);
		const double enlargement1 = extboxarea1 - leftarea;

		const double rightarea = area(right.box);
		const double extboxarea2 = area(box2);
		const double enlargement2 = extboxarea2 - rightarea;

		bool force_left = left.entries.size() < MIN_ENTRIES;
		bool force_right = right.entries.size() < MIN_ENTRIES;

		bool choose_left;

		if (force_left && !force_right) {
			choose_left = true;
		}
		else if (force_right && !force_left) {
			choose_left = false;
		}
		else {
			if (enlargement1 < enlargement2) {
				choose_left = true;
			}
			else if (enlargement2 < enlargement1) {
				choose_left = false;
			}
			else if (leftarea == rightarea) {
				choose_left = left.entries.size() <= right.entries.size();
			}
			else {
				choose_left = leftarea < rightarea;
			}
		}

		if (choose_left) {
			left.box = box1;
			left.entries.push_back(ent);
		}
		else {
			right.box = box2;
			right.entries.push_back(ent);
		}
	}

	data[node_idx] = left;
	data.push_back(right); // oldnode can be realloced after this
	new_idx = data.size() - 1;
	if (node_idx == root)
	{
		node newRoot = node::create_branch();

		newRoot.entries.push_back(entry{ left.box, {}, node_idx });
		expand(newRoot.box, left.box);

		newRoot.entries.push_back(entry{ right.box, {}, new_idx });
		expand(newRoot.box, right.box);

		data.push_back(std::move(newRoot));
		root = uint32_t(data.size() - 1);

		data[node_idx].parent = root;
		data[new_idx].parent = root;
	}
	else {
		auto& parent = data[right.parent];
		parent.entries.push_back(entry{ right.box, {}, new_idx }); // parent.box will be updated in adjust_tree
		for (const auto& ent : right.entries) {
			if (ent.child != node::INVALID_NODE) {
				data[ent.child].parent = new_idx;
			}
		}
	}
}

template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::adjust_tree(uint32_t node_idx, uint32_t new_node_idx)
{
	if (node_idx == root) {
		return;
	}

	uint32_t parent_idx = data[node_idx].parent;

	for (auto& entry : data[parent_idx].entries) {
		if (entry.child == node_idx) {
			entry.box = data[node_idx].box;
			break;
		}
	}

	uint32_t newParentIdx = node::INVALID_NODE;
	if (split_node(parent_idx, newParentIdx)) {
		adjust_tree(parent_idx, newParentIdx);
	}
	else {
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

template<class T, class KEY, class POLICY>
std::vector<T> ds::rtree<T, KEY, POLICY>::query(const KEY& point, std::function<void(const node&, uint32_t)> func) const
{
	if (data.empty()) return {};

	std::vector<T> results;
	query_recursive(root, point, results, func, 0);
	return results;
}

template<class T, class KEY, class POLICY>
std::vector<T> ds::rtree<T, KEY, POLICY>::query(const KEY& box) const
{
	if (data.empty()) return {};

	std::vector<T> results;
	query_recursive(root, box, results);
	return results;
}

template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::query_recursive(uint32_t index, const KEY& area, std::vector<T>& result) const
{
	const auto& node = data[index];

	if (!intersects(node.box, area)) return;

	if (node::is_leaf(node)) {
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

template<class T, class KEY, class POLICY>
void ds::rtree<T, KEY, POLICY>::query_recursive(uint32_t index, const KEY& area, std::vector<T>& result, const std::function<void(const node&, uint32_t)>& func, uint32_t depth) const
{
	const auto& node = data[index];

	if (!intersects(node.box, area)) return;

	func(node, depth++);

	if (node::is_leaf(node)) {
		for (const auto& leaf : node.entries) {
			if (!intersects(leaf.box, area)) continue;

			result.push_back(leaf.val);
		}
	}
	else {
		for (const auto& ent : node.entries) {
			query_recursive(ent.child, area, result, func, depth);
		}
	}
}