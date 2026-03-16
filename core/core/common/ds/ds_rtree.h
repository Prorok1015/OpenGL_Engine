#pragma once
#include "ds_fixed_vector.hpp"
#include <vector>
#include <variant>
#include <glm/glm.hpp>

namespace ds
{
	template<uint32_t MAX, uint32_t MIN = (MAX / 2)>
	struct quadratic_split_policy
	{
		static constexpr uint32_t MAX_ENTRIES = MAX;
		static constexpr uint32_t MIN_ENTRIES = MIN;

		template<class NODE>
		typename NODE::index_type split(std::vector<NODE>& data, typename NODE::index_type current, typename NODE::index_type& root) const
		{
			const auto& oldnode = data[current];
			const auto& entries = oldnode.entries;

			auto seed1 = NODE::INVALID_NODE;
			auto seed2 = NODE::INVALID_NODE;

			// quadratic split algorithm
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

			auto left = oldnode.is_leaf() ? NODE::create_leaf() : NODE::create_branch();
			auto right = oldnode.is_leaf() ? NODE::create_leaf() : NODE::create_branch();
			
			right.parent = left.parent = oldnode.parent;

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

				bool force_left = left.size() < MIN_ENTRIES;
				bool force_right = right.size() < MIN_ENTRIES;

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
						choose_left = left.size() <= right.size();
					} else {
						choose_left = leftarea < rightarea;
					}
				}

				if (choose_left) {
					left.box = box1;
					left.add(ent);
				} else {
					right.box = box2;
					right.add(ent);
				}
			}

			data[current] = left;
			data.push_back(right); // oldnode can be realloced after this
			auto new_idx = data.size() - 1;

			if (current == root)
			{
				NODE newRoot = NODE::create_branch();

				newRoot.add_as_child(left.box, current);
				expand(newRoot.box, left.box);

				newRoot.add_as_child(right.box, new_idx);
				expand(newRoot.box, right.box);

				data.push_back(newRoot);
				root = typename NODE::index_type(data.size() - 1);

				data[current].parent = root;
				data[new_idx].parent = root;
			}
			else {
				auto& parent = data[right.parent];
				parent.add_as_child(right.box, new_idx); // parent.box will be updated in adjust_tree

				if (!right.is_leaf()) {
					for (const auto& ent : right.entries) {
						if (ent.get_child() != NODE::INVALID_NODE) {
							data[ent.get_child()].parent = new_idx;
						}
					}
				}
			}

			return new_idx;
		}
	};

	template<size_t MAX_ENTRIES>
	struct data_storage_policy
	{
		using index_type = uint32_t;
		static constexpr auto INVALID_NODE = std::numeric_limits<index_type>::max();

		template<class T, class KEY>
		struct entry_t
		{
			struct payload_wrapper { T value; };
			using entry_value = std::variant<payload_wrapper, index_type>;

			KEY box;
			entry_value val;

			static entry_t create_with_value(const KEY& k, const T& v) {
				return entry_t{ .box = k, .val = payload_wrapper{ v } };
			}

			static entry_t create_with_child(const KEY& k, index_type idx) {
				return entry_t{ .box = k, .val = idx };
			}

			const T& get_value() const { return std::get<payload_wrapper>(val).value; }
			index_type get_child() const { return std::get<index_type>(val); }
		};

		template<class T, class KEY>
		struct node_t
		{
			static constexpr auto INVALID_NODE = data_storage_policy::INVALID_NODE;
			using entry = entry_t<T, KEY>;
			using index_type = data_storage_policy::index_type;

			KEY box;
			ds::fixed_vector<entry, MAX_ENTRIES + 1> entries;
			bool is_leaf_flag = true;
			index_type parent = INVALID_NODE;

			void add(const entry& ent) { entries.push_back(ent); }
			void add_as_leaf(const KEY& k, const T& v) { entries.push_back(entry::create_with_value(k, v)); }
			void add_as_child(const KEY& k, index_type idx) { entries.push_back(entry::create_with_child(k, idx)); }
			bool is_leaf() const { return is_leaf_flag; }

			size_t size() const { return entries.size(); }

			static node_t create_leaf() { return node_t{}; }
			static node_t create_branch() { return node_t{ .is_leaf_flag = false }; }
		};
	};

	/*
		needs to implemet functions for KEY type:
		 - void expand(KEY& box, const KEY& other);
		 - bool intersects(const KEY& a, const KEY& b);
		 - double area(const KEY& box);
		 - point2d center(const KEY& box);
	*/
	template<class T, class KEY, class POLICY, class STORAGE_POLICY>
	struct rtree : protected POLICY, protected STORAGE_POLICY
	{
		using POLICY::MAX_ENTRIES;
		using POLICY::MIN_ENTRIES;

		using index_type = STORAGE_POLICY::index_type;
		using STORAGE_POLICY::INVALID_NODE;

		using node = typename STORAGE_POLICY::template node_t<T, KEY>;
		using entry = typename STORAGE_POLICY::template entry_t<T, KEY>;

		std::vector<node> data;
		index_type root = 0;

		void insert(const KEY& box, const T& val);
		void adjust_tree(index_type node_idx, index_type new_node_idx = node::INVALID_NODE);

		void build(std::vector<std::pair<KEY, T>> items);
		std::vector<T> query(const KEY& box) const;

	private:
		index_type choose_subtree(const KEY& box, index_type start_idx) const;
		bool split_node(index_type node_idx, index_type& new_idx);
		void insert_recursive(index_type node_idx, const KEY& box, const T& val);
		void query_recursive(index_type nodeIndex, const KEY& box, std::vector<T>& results) const;
	};


	template<class T, class K, class S> 
	using rtree_dsp_t = rtree<T, K, S, data_storage_policy<S::MAX_ENTRIES>>;

	template<class T, class K> 
	using rtree_q = rtree_dsp_t<T, K, quadratic_split_policy<3>>;
}

#include "ds_rtree.hpp"