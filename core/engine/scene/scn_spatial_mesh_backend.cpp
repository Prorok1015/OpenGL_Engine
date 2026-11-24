#include "scn_spatial_detail.h"
#include <snk/common/ds/ds_aabb3.hpp>
#include <snk/common/ds/ds_bvh.hpp>
#include <snk/common/ds/ds_intersect.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_mesh_nodes.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_spatial.h>
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// The mesh backend of the spatial queries (EPIC-81): a ray against the triangles of the geometry the world's meshes hold.
//
// Two levels of ds::bvh. Every distinct (geometry, index range, base vertex) has one triangle tree over the 3D boxes of its
// triangles, built from the "position" element of the loaded geometry (geometry_desc::bounds and its rtree are boxes in UV
// space, not positions); entities that draw the same range share it. Above them one world tree over the boxes of the
// entities (a triangle tree's box through the entity's matrix), refit when the transform system says an entity moved.

namespace {

	// More moved entities than this fraction of all of them: the world tree is built again instead of refit one by one.
	constexpr float k_rebuild_moved_fraction = 0.25f;
	// A refit tree whose inner boxes grew past this multiple of what a fresh build had is built again (ds::bvh::degradation).
	constexpr float k_rebuild_degradation = 2.0f;
	// A geometry range with more triangles than this is tested by the boxes of its triangles only, never by the triangles:
	// the tree of a very large mesh would cost more memory than a click is worth. (The tree still holds the boxes.)
	constexpr std::size_t k_max_exact_triangles = 1'000'000;

	struct tree_key
	{
		const rnd::geometry_desc* geometry = nullptr;
		std::size_t vx_begin = 0;
		std::size_t ind_begin = 0;
		std::size_t ind_end = 0;

		friend bool operator==(const tree_key&, const tree_key&) = default;
	};

	struct tree_key_hash
	{
		std::size_t operator()(const tree_key& key) const
		{
			std::size_t h = std::hash<const void*>{}(key.geometry);
			for (const std::size_t part : { key.vx_begin, key.ind_begin, key.ind_end }) {
				h ^= std::hash<std::size_t>{}(part) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
			}
			return h;
		}
	};

	// The triangles of one range of one geometry.
	struct triangle_tree
	{
		// Keeps the geometry alive, so the pointer in the key names it for as long as the tree lives.
		std::shared_ptr<rnd::geometry_desc> geometry;
		std::size_t vx_begin = 0;
		std::uint32_t position_offset = 0;
		std::uint32_t stride = 0;
		ds::bvh<std::uint32_t> triangles;
		// Every triangle's box together, in the mesh's own space.
		ds::aabb3 box;
		// Too many triangles to test them one by one: only the boxes answer.
		bool boxes_only = false;
	};

	const rnd::driver::BufferElement* position_element(const rnd::driver::BufferLayout& layout)
	{
		for (const auto& element : layout) {
			if (element.Name == "position" && element.Type == rnd::driver::SHADER_DATA_TYPE::VEC3_F) {
				return &element;
			}
		}
		return nullptr;
	}

	bool read_position(const triangle_tree& tree, std::uint32_t index, glm::vec3& out)
	{
		const rnd::geometry_desc& geometry = *tree.geometry;
		const std::size_t vertex = tree.vx_begin + index;
		const std::size_t at = vertex * tree.stride + tree.position_offset;
		if (at + sizeof(glm::vec3) > geometry.vertices.size()) {
			return false;
		}
		std::memcpy(&out, geometry.vertices.data() + at, sizeof(glm::vec3));
		return true;
	}

	// Null when the geometry has no float3 "position" or no usable range.
	std::shared_ptr<triangle_tree> build_tree(std::shared_ptr<rnd::geometry_desc> geometry, const scn::mesh_view& view)
	{
		const auto* position = position_element(geometry->layout);
		const std::uint32_t stride = geometry->layout.get_stride();
		if (!position || stride == 0 || position->Offset + sizeof(glm::vec3) > stride) {
			return nullptr;
		}
		auto tree = std::make_shared<triangle_tree>();
		tree->geometry = std::move(geometry);
		tree->vx_begin = view.vx_begin;
		tree->position_offset = static_cast<std::uint32_t>(position->Offset);
		tree->stride = stride;
		const auto& indices = tree->geometry->indices;
		const std::size_t end = std::min(view.ind_end, indices.size());
		std::vector<ds::bvh<std::uint32_t>::item> items;
		if (end > view.ind_begin) {
			items.reserve((end - view.ind_begin) / 3);
		}
		std::size_t skipped = 0;
		for (std::size_t i = view.ind_begin; i + 2 < end; i += 3) {
			glm::vec3 corner[3];
			bool whole = true;
			for (std::size_t k = 0; k < 3; ++k) {
				whole = whole && read_position(*tree, indices[i + k], corner[k]);
			}
			whole = whole && std::isfinite(corner[0].x + corner[0].y + corner[0].z + corner[1].x + corner[1].y + corner[1].z
				+ corner[2].x + corner[2].y + corner[2].z);
			if (!whole) {
				++skipped;
				continue;
			}
			ds::aabb3 box;
			for (const glm::vec3& point : corner) {
				ds::expand(box, point);
			}
			items.push_back({ box, static_cast<std::uint32_t>(i) });
		}
		if (skipped != 0) {
			egLOG_WARN("scene/spatial", "a geometry has {} triangles with a NaN or infinite position or an index out of range: ray queries skip them", skipped);
		}
		tree->boxes_only = items.size() > k_max_exact_triangles;
		tree->triangles.build(std::move(items));
		tree->box = tree->triangles.bounds();
		return tree;
	}

	// One mesh entity of the world.
	struct entry
	{
		entt::entity entity = entt::null;
		std::shared_ptr<triangle_tree> tree;
		scn::mesh_view view{};
		const rnd::geometry_desc* geometry = nullptr;
		glm::mat4 world{ 1.f };
		glm::mat4 inverse{ 1.f };
		glm::mat3 normal_matrix{ 1.f };
		// The matrix turns the triangles inside out: what is a front face in the mesh is a back face in the world.
		bool mirrored = false;
		// The matrix cannot be inverted: nothing of it is hit.
		bool degenerate = false;

		void set_world(const glm::mat4& matrix)
		{
			world = matrix;
			const glm::mat3 linear{ matrix };
			const float determinant = glm::determinant(linear);
			degenerate = !(std::abs(determinant) > 1e-12f);
			mirrored = determinant < 0.f;
			if (degenerate) {
				inverse = glm::mat4{ 1.f };
				normal_matrix = glm::mat3{ 1.f };
				return;
			}
			inverse = glm::inverse(matrix);
			normal_matrix = glm::transpose(glm::mat3{ inverse });
		}

		// An entry nothing can hit still needs a box in the world tree: a point at the origin keeps the tree's maths finite.
		ds::aabb3 world_box() const
		{
			const ds::aabb3 box = tree && !degenerate ? ds::transformed(tree->box, world) : ds::aabb3{};
			return box.empty() ? ds::aabb3{ glm::vec3{ 0.f }, glm::vec3{ 0.f } } : box;
		}
	};

	bool excluded(std::span<const entt::entity> list, entt::entity entity)
	{
		return std::find(list.begin(), list.end(), entity) != list.end();
	}

	// How a query wants the answer.
	enum class mode { closest, any };

	class mesh_backend final : public scn::detail::spatial_backend
	{
	public:
		std::optional<scn::ray_hit> raycast_closest(entt::registry& world, const scn::ray& line, const scn::ray_filter& filter) override
		{
			std::optional<scn::ray_hit> best;
			walk(world, line, filter, mode::closest, [&best](const scn::ray_hit& hit) {
				best = hit;
				return hit.distance;
			});
			return best;
		}

		bool raycast_any(entt::registry& world, const scn::ray& line, const scn::ray_filter& filter) override
		{
			bool found = false;
			walk(world, line, filter, mode::any, [&found](const scn::ray_hit&) {
				found = true;
				return 0.f;
			});
			return found;
		}

		void raycast_all(entt::registry& world, const scn::ray& line, const scn::ray_filter& filter, std::vector<scn::ray_hit>& out) override
		{
			const std::size_t first = out.size();
			walk(world, line, filter, mode::closest, [&out](const scn::ray_hit& hit) {
				out.push_back(hit);
				// The walk goes on past this entity to every other: the distance is not narrowed.
				return std::numeric_limits<float>::infinity();
			});
			std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
				[](const scn::ray_hit& a, const scn::ray_hit& b) {
					return a.distance != b.distance ? a.distance < b.distance : a.entity < b.entity;
				});
		}

		void entities_moved(entt::registry& world, std::span<const entt::entity> moved) override
		{
			if (!m_built) {
				return;
			}
			std::vector<entt::entity> unique(moved.begin(), moved.end());
			std::sort(unique.begin(), unique.end());
			unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
			std::size_t refit = 0;
			for (const entt::entity entity : unique) {
				const auto found = m_index_of.find(entity);
				if (found == m_index_of.end()) {
					continue;
				}
				const auto* transform = world.try_get<scn::world_transform>(entity);
				if (!transform) {
					continue;
				}
				entry& moving = m_entries[found->second];
				moving.set_world(transform->world);
				m_world.set_box(found->second, moving.world_box());
				++refit;
			}
			if (static_cast<float>(refit) > k_rebuild_moved_fraction * static_cast<float>(m_entries.size())
				|| m_world.degradation() > k_rebuild_degradation) {
				m_world.rebuild();
			}
		}

		void structure_changed() override { m_dirty = true; }

	private:
		// Calls `found(hit)` for the nearest hit of every entity the ray meets; what it returns is the distance the walk goes on
		// to (infinity: every entity; the hit's own distance: only nearer ones). For `any` the walk stops at the first hit.
		template <class Found>
		void walk(entt::registry& world, const scn::ray& line, const scn::ray_filter& filter, mode how, Found&& found)
		{
			const float length = glm::length(line.direction);
			if (!(length > 0.f) || !(filter.max_distance > 0.f)) {
				return;
			}
			ensure_current(world);
			const glm::vec3 direction = line.direction / length;
			const ds::ray3 ray(line.origin, direction);
			float t_max = filter.max_distance;
			bool stopped = false;
			m_world.traverse_ray(ray, t_max, [&](std::uint32_t, const std::uint32_t& index, float) {
				if (stopped) {
					return ds::bvh_ray_action{ t_max, true };
				}
				const auto hit = test_entry(world, m_entries[index], ray, filter, t_max, how);
				if (!hit) {
					return ds::bvh_ray_action{ t_max, false };
				}
				const float next = found(*hit);
				if (how == mode::any) {
					stopped = true;
					return ds::bvh_ray_action{ t_max, true };
				}
				t_max = std::min(t_max, next);
				return ds::bvh_ray_action{ t_max, false };
			});
		}

		// The nearest hit inside one entity, after the filters.
		std::optional<scn::ray_hit> test_entry(entt::registry& world, const entry& mesh, const ds::ray3& ray, const scn::ray_filter& filter,
			float t_max, mode how)
		{
			if (mesh.degenerate || !mesh.tree || !world.valid(mesh.entity) || excluded(filter.exclude, mesh.entity)) {
				return std::nullopt;
			}
			if (!filter.include_hidden && !world.all_of<scn::renderable>(mesh.entity)) {
				return std::nullopt;
			}
			const auto* layers = world.try_get<scn::spatial_layers>(mesh.entity);
			const scn::spatial_mask on = layers ? layers->mask : scn::spatial_mask{ scn::k_default_spatial_layer };
			if (!on.any_of(filter.mask)) {
				return std::nullopt;
			}
			const glm::vec3 local_origin = glm::vec3(mesh.inverse * glm::vec4(ray.origin, 1.f));
			const glm::vec3 local_direction = glm::vec3(mesh.inverse * glm::vec4(ray.direction, 0.f));
			if (!(glm::dot(local_direction, local_direction) > 0.f)) {
				return std::nullopt;
			}
			const ds::ray3 local(local_origin, local_direction);
			// A point of the local ray is the same parameter t of the world ray: the transform is affine.
			struct best_hit
			{
				float t = 0.f;
				std::uint32_t first_index = ~0u;
				ds::triangle_hit triangle{};
				bool exact = false;
				bool found = false;
			} best;
			best.t = t_max;
			const triangle_tree& tree = *mesh.tree;
			tree.triangles.traverse_ray(local, t_max, [&](std::uint32_t, const std::uint32_t& first_index, float box_entry) {
				if (!filter.triangles || tree.boxes_only) {
					const float entry_distance = std::max(box_entry, 0.f);
					if (entry_distance <= best.t) {
						best = { entry_distance, first_index, {}, false, true };
					}
					return ds::bvh_ray_action{ best.t, how == mode::any && best.found };
				}
				const auto& indices = tree.geometry->indices;
				glm::vec3 a;
				glm::vec3 b;
				glm::vec3 c;
				if (!read_position(tree, indices[first_index], a) || !read_position(tree, indices[first_index + 1], b)
					|| !read_position(tree, indices[first_index + 2], c)) {
					return ds::bvh_ray_action{ best.t, false };
				}
				const auto hit = ds::ray_triangle(local.origin, local.direction, a, b, c, false, best.t);
				if (!hit) {
					return ds::bvh_ray_action{ best.t, false };
				}
				const bool seen_from_behind = hit->back_face != mesh.mirrored;
				if (filter.cull_back_faces && seen_from_behind) {
					return ds::bvh_ray_action{ best.t, false };
				}
				best = { hit->t, first_index, *hit, true, true };
				return ds::bvh_ray_action{ best.t, how == mode::any };
			});
			if (!best.found) {
				return std::nullopt;
			}
			scn::ray_hit out;
			out.entity = mesh.entity;
			out.distance = best.t;
			out.point = ray.origin + ray.direction * best.t;
			if (best.exact) {
				const auto& indices = tree.geometry->indices;
				glm::vec3 a;
				glm::vec3 b;
				glm::vec3 c;
				read_position(tree, indices[best.first_index], a);
				read_position(tree, indices[best.first_index + 1], b);
				read_position(tree, indices[best.first_index + 2], c);
				glm::vec3 normal = mesh.normal_matrix * glm::cross(b - a, c - a);
				const float normal_length = glm::length(normal);
				normal = normal_length > 0.f ? normal / normal_length : -ray.direction;
				// On the side the ray came from; back_face says whether that is the triangle's back.
				if (glm::dot(normal, ray.direction) > 0.f) {
					normal = -normal;
				}
				out.normal = normal;
				out.triangle = best.first_index / 3;
				out.barycentric = { best.triangle.u, best.triangle.v };
				out.back_face = best.triangle.back_face != mesh.mirrored;
			} else {
				out.normal = -ray.direction;
			}
			return out;
		}

		// An entity that no longer draws the range or the geometry its entry was built from (the held handle was given another
		// geometry: a retag, a reload held again).
		static bool stale(entt::registry& world, const entry& mesh)
		{
			const auto* component = world.try_get<scn::mesh_component>(mesh.entity);
			if (!component || component->mesh.vx_begin != mesh.view.vx_begin || component->mesh.ind_begin != mesh.view.ind_begin
				|| component->mesh.ind_end != mesh.view.ind_end) {
				return true;
			}
			const auto* holds = world.ctx().find<scn::geometry_holds>();
			const res::res_handle<rnd::geometry_desc>* held = holds ? holds->find(mesh.entity) : nullptr;
			return !held || !held->is_ready() || held->get().get() != mesh.geometry;
		}

		// Builds the trees again when a mesh or geometry component came, went or changed (structure_changed), when the world has
		// another number of meshes than the last build saw, when an entry no longer matches what its entity draws, or when a
		// geometry that was not loaded is now. Before the traversal: a walk never sees a half-stale tree.
		void ensure_current(entt::registry& world)
		{
			const std::size_t meshes = world.view<const scn::mesh_component, const scn::world_transform>().size_hint();
			bool rebuild = !m_built || m_dirty || meshes != m_mesh_count;
			if (!rebuild) {
				rebuild = std::ranges::any_of(m_entries, [&world](const entry& mesh) { return stale(world, mesh); });
			}
			if (!rebuild && !m_pending.empty()) {
				const auto* holds = world.ctx().find<scn::geometry_holds>();
				rebuild = holds && std::ranges::any_of(m_pending, [&](entt::entity entity) {
					const auto* held = holds->find(entity);
					return held && held->is_ready();
				});
			}
			if (rebuild) {
				build(world);
			}
		}

		void build(entt::registry& world)
		{
			m_entries.clear();
			m_index_of.clear();
			m_pending.clear();
			std::vector<ds::bvh<std::uint32_t>::item> items;
			const auto* holds = world.ctx().find<scn::geometry_holds>();
			std::unordered_map<tree_key, std::shared_ptr<triangle_tree>, tree_key_hash> used;
			const auto view = world.view<const scn::mesh_component, const scn::world_transform>();
			m_mesh_count = view.size_hint();
			for (const entt::entity entity : view) {
				const scn::mesh_view& range = view.get<const scn::mesh_component>(entity).mesh;
				const res::res_handle<rnd::geometry_desc>* held = holds ? holds->find(entity) : nullptr;
				if (!held) {
					continue;
				}
				entry mesh;
				mesh.entity = entity;
				mesh.view = range;
				mesh.set_world(view.get<const scn::world_transform>(entity).world);
				if (!held->is_ready()) {
					m_pending.push_back(entity);
				} else {
					std::shared_ptr<rnd::geometry_desc> geometry = held->get();
					mesh.geometry = geometry.get();
					const tree_key key{ mesh.geometry, range.vx_begin, range.ind_begin, range.ind_end };
					auto cached = m_trees.find(key);
					if (cached == m_trees.end()) {
						cached = m_trees.emplace(key, build_tree(std::move(geometry), range)).first;
					}
					mesh.tree = cached->second;
					if (!mesh.tree) {
						// Not a geometry with float3 positions: never hit, never asked again until it changes.
						egLOG_DEBUG("scene/spatial", "mesh {} has no float3 position: it is skipped by ray queries", static_cast<std::uint32_t>(entity));
					}
					used.emplace(key, mesh.tree);
				}
				m_index_of[entity] = static_cast<std::uint32_t>(m_entries.size());
				items.push_back({ mesh.world_box(), static_cast<std::uint32_t>(m_entries.size()) });
				m_entries.push_back(std::move(mesh));
			}
			// Trees nobody draws any more go; the ones in use are kept for the next build.
			m_trees = std::move(used);
			m_world.build(std::move(items));
			m_built = true;
			m_dirty = false;
		}

		bool m_built = false;
		bool m_dirty = false;
		std::size_t m_mesh_count = 0;
		std::vector<entry> m_entries;
		std::unordered_map<entt::entity, std::uint32_t> m_index_of;
		std::vector<entt::entity> m_pending;
		std::unordered_map<tree_key, std::shared_ptr<triangle_tree>, tree_key_hash> m_trees;
		ds::bvh<std::uint32_t> m_world;
	};

}

std::unique_ptr<scn::detail::spatial_backend> scn::detail::make_mesh_backend()
{
	return std::make_unique<mesh_backend>();
}
