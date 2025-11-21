#pragma once
#include <vector>
#include <glm/glm.hpp>

namespace ds
{
	using point2d = glm::vec2;

	struct bbox
	{
		union
		{
			point2d p[2];
			struct { point2d min; point2d max; };
		};
	};
	
	using triangle = uint32_t;

	template<class T, class KEY, class SUBKEY>
	struct rtree
	{
		static constexpr auto MAX_ENTRIES = 3;
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
		};

		std::vector<node> data{ 1 };

		uint32_t choose_node(uint32_t, T&&);

		KEY calculate_leaf_box(node& nd);

		void update_box_upwards(uint32_t node_idx) {
			while (true) {
				node& n = data[node_idx];
				if (n.entries.empty()) { node_idx = n.parent; continue; }

				n.box = n.entries[0].box;
				for (size_t i = 1; i < n.entries.size(); ++i)
					n.box = unite(n.box, n.entries[i].box);

				if (node_idx == 0) break;
				node_idx = n.parent;
			}
		}

		void insert(KEY&& box, T&& val)
		{
			uint32_t node_id = 0;
			while (data[node_id].is_leaf)
				node_id = choose_node(box);

			auto& leaf = data[node_id];
			leaf.entries.push_back({ box, val });
			leaf.box = calculate_leaf_box(leaf);


		}
		void remove(KEY&&);
		std::vector<T> query(KEY&&);
		const T& find(SUBKEY&&);
	};
}