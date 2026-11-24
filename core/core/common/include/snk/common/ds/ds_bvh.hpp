#pragma once
#include "ds_aabb3.hpp"
#include "ds_intersect.hpp"
#include <snk/common/engine_assert.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace ds {
	// What a ray visitor answers: the distance the walk goes on to (it may only shrink: a closest-hit search narrows it to the
	// hit it found), and whether to stop at once (an any-hit search).
	struct bvh_ray_action final
	{
		float t_max = std::numeric_limits<float>::infinity();
		bool stop = false;
	};

	// A bounding volume hierarchy over boxes, each carrying a value of type T. It knows no ECS and no render: the caller
	// fills it (build), moves boxes (set_box: a refit, O(depth)), rebuilds it when it got bad (degradation), and walks it
	// culled by a ray, a box or a frustum. Items are named by their index in the vector given to build (an `id`), which
	// stays valid until the next build. rtree (ds_rtree.h) stays the structure for insert and remove one by one; this one
	// is for boxes that move every frame.
	//
	// Not thread safe for writes; any number of threads may walk it while nothing writes.
	template <class T>
	class bvh final
	{
	public:
		struct item final
		{
			aabb3 box;
			T value;
		};

		// At most this many items sit in one leaf (more only when their centres coincide and cannot be split).
		static constexpr std::uint32_t k_leaf_size = 4;

		bvh() = default;
		explicit bvh(std::vector<item> items) { build(std::move(items)); }

		// Replaces the content. The tree is a median split on the longest axis of the centres: balanced, depth <= log2(n).
		void build(std::vector<item> items)
		{
			m_items = std::move(items);
			rebuild();
		}

		// Builds the tree again over the current boxes (after many set_box calls); the ids do not change.
		void rebuild()
		{
			for (const item& each : m_items) {
				ASSERT_MSG(finite(each.box), "bvh: a box has a NaN or infinite corner");
			}
			m_nodes.clear();
			m_order.resize(m_items.size());
			for (std::uint32_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
			m_leaf_of.assign(m_items.size(), 0);
			m_inner_area = 0.f;
			if (m_items.empty()) {
				m_baseline_area = 0.f;
				return;
			}
			m_nodes.reserve(2 * m_items.size() / k_leaf_size + 1);
			m_nodes.push_back({});
			build_node(0, 0, static_cast<std::uint32_t>(m_items.size()), k_no_parent);
			m_baseline_area = m_inner_area;
		}

		std::size_t size() const { return m_items.size(); }
		bool empty() const { return m_items.empty(); }
		const item& at(std::uint32_t id) const { return m_items[id]; }
		// The box of everything (empty when there is nothing).
		aabb3 bounds() const { return m_nodes.empty() ? aabb3{} : m_nodes[0].box; }

		// Moves an item's box and refits the boxes above it, stopping where a box did not change.
		void set_box(std::uint32_t id, const aabb3& box)
		{
			ASSERT_MSG(id < m_items.size(), "bvh::set_box: id {} of {} items", id, m_items.size());
			ASSERT_MSG(finite(box), "bvh::set_box: the box has a NaN or infinite corner");
			m_items[id].box = box;
			std::uint32_t node = m_leaf_of[id];
			refit_leaf(node);
			while (m_nodes[node].parent != k_no_parent) {
				node = m_nodes[node].parent;
				if (!refit_inner(node)) break;
			}
		}

		// How much worse the tree got since the last build: the sum of the areas of the inner boxes over what it was right
		// after the build (1 = as good as new). A caller rebuilds past a limit of its own.
		float degradation() const { return m_baseline_area > 0.f ? m_inner_area / m_baseline_area : 1.f; }

		// Walks the items a ray can reach, the nearer child first. visit(id, value, entry) is called with the distance at
		// which the ray enters the item's own box and answers a bvh_ray_action.
		template <class Visit>
		void traverse_ray(const ray3& ray, float t_max, Visit&& visit) const
		{
			if (m_nodes.empty()) return;
			std::array<std::pair<std::uint32_t, float>, k_stack_size> stack;
			std::size_t top = 0;
			if (const auto entry = ray_aabb(ray, m_nodes[0].box, t_max)) stack[top++] = { 0, *entry };
			while (top > 0) {
				const auto [index, entry] = stack[--top];
				if (entry > t_max) continue;
				const node& n = m_nodes[index];
				if (n.is_leaf()) {
					for (std::uint32_t k = n.first; k < n.first + n.count; ++k) {
						const std::uint32_t id = m_order[k];
						const auto own = ray_aabb(ray, m_items[id].box, t_max);
						if (!own) continue;
						const bvh_ray_action action = visit(id, m_items[id].value, *own);
						if (action.stop) return;
						t_max = std::min(t_max, action.t_max);
					}
					continue;
				}
				const auto left = ray_aabb(ray, m_nodes[n.first].box, t_max);
				const auto right = ray_aabb(ray, m_nodes[n.first + 1].box, t_max);
				// The farther one is pushed first so the nearer one is popped first.
				if (left && right) {
					const bool left_first = *left <= *right;
					stack[top++] = left_first ? std::pair{ n.first + 1, *right } : std::pair{ n.first, *left };
					stack[top++] = left_first ? std::pair{ n.first, *left } : std::pair{ n.first + 1, *right };
				} else if (left) {
					stack[top++] = { n.first, *left };
				} else if (right) {
					stack[top++] = { n.first + 1, *right };
				}
			}
		}

		// Calls visit(id, value) for every item whose box intersects `box`; visit answers false to stop.
		template <class Visit>
		void query_box(const aabb3& box, Visit&& visit) const
		{
			if (m_nodes.empty() || !intersects(m_nodes[0].box, box)) return;
			std::array<std::uint32_t, k_stack_size> stack;
			std::size_t top = 0;
			stack[top++] = 0;
			while (top > 0) {
				const node& n = m_nodes[stack[--top]];
				if (n.is_leaf()) {
					for (std::uint32_t k = n.first; k < n.first + n.count; ++k) {
						const std::uint32_t id = m_order[k];
						if (intersects(m_items[id].box, box) && !visit(id, m_items[id].value)) return;
					}
					continue;
				}
				for (std::uint32_t child = n.first; child < n.first + 2; ++child) {
					if (intersects(m_nodes[child].box, box)) stack[top++] = child;
				}
			}
		}

		// Calls visit(id, value, fully_inside) for every item whose box is inside or crosses the frustum; a node inside
		// it is not tested again below. visit answers false to stop.
		template <class Visit>
		void query_frustum(const frustum& view, Visit&& visit) const
		{
			if (m_nodes.empty()) return;
			std::array<std::pair<std::uint32_t, bool>, k_stack_size> stack;
			std::size_t top = 0;
			const frustum_test root = test_frustum(view, m_nodes[0].box);
			if (root == frustum_test::outside) return;
			stack[top++] = { 0, root == frustum_test::inside };
			while (top > 0) {
				const auto [index, inside] = stack[--top];
				const node& n = m_nodes[index];
				if (n.is_leaf()) {
					for (std::uint32_t k = n.first; k < n.first + n.count; ++k) {
						const std::uint32_t id = m_order[k];
						if (m_items[id].box.empty()) continue;
						bool fully = inside;
						if (!inside) {
							const frustum_test own = test_frustum(view, m_items[id].box);
							if (own == frustum_test::outside) continue;
							fully = own == frustum_test::inside;
						}
						if (!visit(id, m_items[id].value, fully)) return;
					}
					continue;
				}
				for (std::uint32_t child = n.first; child < n.first + 2; ++child) {
					if (inside) {
						stack[top++] = { child, true };
						continue;
					}
					const frustum_test test = test_frustum(view, m_nodes[child].box);
					if (test != frustum_test::outside) stack[top++] = { child, test == frustum_test::inside };
				}
			}
		}

	private:
		static constexpr std::uint32_t k_no_parent = ~0u;
		// Depth <= 32 for 2^32 items, two entries pushed per level.
		static constexpr std::size_t k_stack_size = 96;

		struct node final
		{
			aabb3 box;
			// Leaf: the first slot in m_order. Inner: the left child (the right one is the next node).
			std::uint32_t first = 0;
			// Leaf: how many items; inner: 0.
			std::uint32_t count = 0;
			std::uint32_t parent = k_no_parent;

			bool is_leaf() const { return count != 0; }
		};

		static float box_area(const aabb3& box) { return surface_area(box); }

		// An empty box is finite (its corners are the largest floats); NaN and infinity would break the median split.
		static bool finite(const aabb3& box)
		{
			return box.empty() || (std::isfinite(box.min.x) && std::isfinite(box.min.y) && std::isfinite(box.min.z)
				&& std::isfinite(box.max.x) && std::isfinite(box.max.y) && std::isfinite(box.max.z));
		}

		void build_node(std::uint32_t index, std::uint32_t first, std::uint32_t count, std::uint32_t parent)
		{
			aabb3 box;
			aabb3 centres;
			for (std::uint32_t k = first; k < first + count; ++k) {
				const aabb3& item_box = m_items[m_order[k]].box;
				expand(box, item_box);
				if (!item_box.empty()) expand(centres, center(item_box));
			}
			m_nodes[index].box = box;
			m_nodes[index].parent = parent;
			const glm::vec3 spread = centres.empty() ? glm::vec3{ 0.f } : ds::size(centres);
			const int axis = spread.x >= spread.y && spread.x >= spread.z ? 0 : (spread.y >= spread.z ? 1 : 2);
			if (count <= k_leaf_size || spread[axis] <= 0.f) {
				m_nodes[index].first = first;
				m_nodes[index].count = count;
				for (std::uint32_t k = first; k < first + count; ++k) m_leaf_of[m_order[k]] = index;
				return;
			}
			const std::uint32_t mid = first + count / 2;
			std::nth_element(m_order.begin() + first, m_order.begin() + mid, m_order.begin() + first + count,
				[this, axis](std::uint32_t a, std::uint32_t b) { return centre_of(a)[axis] < centre_of(b)[axis]; });
			const std::uint32_t left = static_cast<std::uint32_t>(m_nodes.size());
			m_nodes.push_back({});
			m_nodes.push_back({});
			m_nodes[index].first = left;
			m_nodes[index].count = 0;
			m_inner_area += box_area(box);
			build_node(left, first, mid - first, index);
			build_node(left + 1, mid, first + count - mid, index);
		}

		glm::vec3 centre_of(std::uint32_t id) const
		{
			const aabb3& box = m_items[id].box;
			return box.empty() ? glm::vec3{ 0.f } : center(box);
		}

		void refit_leaf(std::uint32_t index)
		{
			node& leaf = m_nodes[index];
			aabb3 box;
			for (std::uint32_t k = leaf.first; k < leaf.first + leaf.count; ++k) expand(box, m_items[m_order[k]].box);
			leaf.box = box;
		}

		// Returns whether the box changed (so the parent has to follow).
		bool refit_inner(std::uint32_t index)
		{
			node& inner = m_nodes[index];
			aabb3 box = m_nodes[inner.first].box;
			expand(box, m_nodes[inner.first + 1].box);
			if (box == inner.box) return false;
			m_inner_area += box_area(box) - box_area(inner.box);
			inner.box = box;
			return true;
		}

		std::vector<item> m_items;
		std::vector<node> m_nodes;
		// The ids in leaf order.
		std::vector<std::uint32_t> m_order;
		std::vector<std::uint32_t> m_leaf_of;
		float m_inner_area = 0.f;
		float m_baseline_area = 0.f;
	};
}
