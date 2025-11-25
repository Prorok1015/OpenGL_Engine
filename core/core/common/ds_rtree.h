#pragma once
#include "ds_fixed_vector.hpp"
#include <vector>
#include <glm/glm.hpp>

namespace ds
{
	template<uint32_t MAX, uint32_t MIN = (MAX / 2)>
	struct quadratic_split_policy
	{
		static constexpr uint32_t MAX_ENTRIES = MAX;
		static constexpr uint32_t MIN_ENTRIES = MIN;

		template<class NODE>
		uint32_t split(std::vector<NODE>& data, uint32_t current, uint32_t& root) const
		{
			// quadratic split algorithm
			const auto& oldnode = data[current];
			const auto& entries = oldnode.entries;

			uint32_t seed1 = NODE::INVALID_NODE;
			uint32_t seed2 = NODE::INVALID_NODE;

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

			ASSERT_MSG(seed1 != NODE::INVALID_NODE && seed2 != NODE::INVALID_NODE, "seeds incorrect!");

			auto left = NODE::is_leaf(oldnode) ? NODE::create_leaf() : NODE::create_branch();
			auto right = NODE::is_leaf(oldnode) ? NODE::create_leaf() : NODE::create_branch();
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

				bool force_left = NODE::size(left) < MIN_ENTRIES;
				bool force_right = NODE::size(right) < MIN_ENTRIES;

				bool choose_left;

				if (force_left && !force_right) {
					choose_left = true;
				} else if (force_right && !force_left) {
					choose_left = false;
				} else {
					if (enlargement1 < enlargement2) {
						choose_left = true;
					} else if (enlargement2 < enlargement1) {
						choose_left = false;
					} else if (leftarea == rightarea) {
						choose_left = NODE::size(left) <= NODE::size(right);
					} else {
						choose_left = leftarea < rightarea;
					}
				}

				if (choose_left) {
					left.box = box1;
					NODE::add_to_leaf(left, ent);
				}
				else {
					right.box = box2;
					NODE::add_to_leaf(right, ent);
				}
			}

			data[current] = left;
			data.push_back(right); // oldnode can be realloced after this
			auto new_idx = data.size() - 1;
			if (current == root)
			{
				NODE newRoot = NODE::create_branch();

				NODE::add_to_child(newRoot, left.box, current);
				expand(newRoot.box, left.box);

				NODE::add_to_child(newRoot, right.box, new_idx );
				expand(newRoot.box, right.box);

				data.push_back(newRoot);
				root = uint32_t(data.size() - 1);

				data[current].parent = root;
				data[new_idx].parent = root;
			}
			else {
				auto& parent = data[right.parent];
				NODE::add_to_child(parent, right.box, new_idx); // parent.box will be updated in adjust_tree
				for (const auto& ent : right.entries) {
					if (ent.child != NODE::INVALID_NODE) {
						data[ent.child].parent = new_idx;
					}
				}
			}

			return new_idx;
		}
	};

	/*
		needs to implemet functions for KEY type:
		 - void expand(KEY& box, const KEY& other);
		 - bool intersects(const KEY& a, const KEY& b);
		 - double area(const KEY& box);
		 - point2d center(const KEY& box);
	*/
	template<class T, class KEY, class POLICY>
	struct rtree : protected POLICY
	{
		using POLICY::MAX_ENTRIES;
		using POLICY::MIN_ENTRIES;

		struct entry
		{
			KEY box;
			T val;
			uint32_t child = node::INVALID_NODE;
		};

		struct node
		{
			static constexpr auto INVALID_NODE = std::numeric_limits<uint32_t>::max();

			KEY box;
			ds::fixed_vector<entry, MAX_ENTRIES + 1> entries;
			bool is_leaf_flag = true;
			uint32_t parent = INVALID_NODE;

			static inline void add_to_leaf(node& n, KEY k, T v) { n.entries.push_back(entry{ .box = k, .val = v }); }
			static inline void add_to_leaf(node& n, const entry& ent) { n.entries.push_back(ent); }
			static inline void add_to_child(node& n, KEY k, uint32_t idx) { n.entries.push_back(entry{.box = k, .child = idx}); }
			static inline bool is_leaf(const node& n) { return n.is_leaf_flag; }

			static inline size_t size(const node& n) { return n.entries.size(); }

			static inline node create_leaf() { return node{}; }
			static inline node create_branch() { return node{ .is_leaf_flag = false }; }
		};

		std::vector<node> data;
		uint32_t root = 0;

		void insert(const KEY& box, const T& val);
		void adjust_tree(uint32_t node_idx, uint32_t new_node_idx = node::INVALID_NODE);

		void build(std::vector<std::pair<KEY, T>> items);
		std::vector<T> query(const KEY& box, std::function<void(const node&, uint32_t)> func) const;
		std::vector<T> query(const KEY& box) const;

	private:
		void split_impl(uint32_t node_idx, uint32_t& new_idx);
		uint32_t choose_subtree(const KEY& box, uint32_t start_idx) const;
		bool split_node(uint32_t node_idx, uint32_t& new_idx);
		void insert_recursive(uint32_t node_idx, const KEY& box, const T& val);
		void query_recursive(uint32_t nodeIndex, const KEY& box, std::vector<T>& results) const;
		void query_recursive(uint32_t nodeIndex, const KEY& box, std::vector<T>& results, const std::function<void(const node&, uint32_t)>& func, uint32_t depth) const;
	};


	template<class T, class K> 
	using rtree_q = rtree<T, K, quadratic_split_policy<64>>;
}

#include "ds_rtree.hpp"