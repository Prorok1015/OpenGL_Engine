#include "scn_spatial_detail.h"
#include <snk/common/ds/ds_aabb3.hpp>
#include <snk/common/ds/ds_bvh.hpp>
#include <snk/common/ds/ds_intersect.hpp>
#include <snk/common/ds/ds_shape.hpp>
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
		tree_key key;
		scn::mesh_view view{};
		const rnd::geometry_desc* geometry = nullptr;
		glm::mat4 world{ 1.f };
		glm::mat4 inverse{ 1.f };
		glm::mat3 normal_matrix{ 1.f };
		// The matrix turns the triangles inside out: what is a front face in the mesh is a back face in the world.
		bool mirrored = false;
		// The matrix cannot be inverted: nothing of it is hit.
		bool degenerate = false;

		// False for a matrix with a NaN or infinite element: nothing of the entity is hit until it has a good one.
		bool set_world(const glm::mat4& matrix)
		{
			world = matrix;
			if (!std::isfinite(glm::dot(matrix[0], matrix[0]) + glm::dot(matrix[1], matrix[1]) + glm::dot(matrix[2], matrix[2])
					+ glm::dot(matrix[3], matrix[3]))) {
				degenerate = true;
				mirrored = false;
				inverse = glm::mat4{ 1.f };
				normal_matrix = glm::mat3{ 1.f };
				return false;
			}
			const glm::mat3 linear{ matrix };
			const float determinant = glm::determinant(linear);
			degenerate = !(std::abs(determinant) > 1e-12f);
			mirrored = determinant < 0.f;
			if (degenerate) {
				inverse = glm::mat4{ 1.f };
				normal_matrix = glm::mat3{ 1.f };
				return true;
			}
			inverse = glm::inverse(matrix);
			normal_matrix = glm::transpose(glm::mat3{ inverse });
			return true;
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

	// Whether an entry is looked at at all by a query with this filter: something to hit, not excluded, visible when hidden ones are not
	// wanted, on a layer of the mask.
	bool passes_filter(const entry& mesh, bool renderable, scn::spatial_mask layers, const scn::ray_filter& filter)
	{
		// A mesh with no triangle has no box of its own: the world tree holds a point at the origin for it, which nothing may hit.
		if (mesh.degenerate || !mesh.tree || mesh.tree->box.empty() || excluded(filter.exclude, mesh.entity)) {
			return false;
		}
		if (!filter.include_hidden && !renderable) {
			return false;
		}
		return layers.any_of(filter.mask);
	}

	// The nearest hit inside one entity, after the filters. `renderable` and `layers` are what the entity has now (the sync
	// queries read them from the registry) or had when the snapshot was taken (the async ones).
	std::optional<scn::ray_hit> test_mesh(const entry& mesh, bool renderable, scn::spatial_mask layers, const ds::ray3& ray,
		const scn::ray_filter& filter, float t_max, mode how)
	{
		if (!passes_filter(mesh, renderable, layers, filter)) {
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

	bool read_triangle(const triangle_tree& tree, std::uint32_t first_index, glm::vec3& a, glm::vec3& b, glm::vec3& c)
	{
		const auto& indices = tree.geometry->indices;
		return read_position(tree, indices[first_index], a) && read_position(tree, indices[first_index + 1], b)
			&& read_position(tree, indices[first_index + 2], c);
	}

	glm::vec3 to_world(const entry& mesh, const glm::vec3& point) { return glm::vec3(mesh.world * glm::vec4(point, 1.f)); }

	// A shape of a query, in the form the shape maths takes.
	struct prepared_shape
	{
		scn::shape_kind kind = scn::shape_kind::sphere;
		ds::sphere3 sphere;
		ds::capsule3 capsule;
		ds::obb3 box;
		ds::aabb3 bounds;
	};

	prepared_shape prepare(const scn::shape& volume)
	{
		prepared_shape out;
		out.kind = volume.kind;
		switch (volume.kind) {
		case scn::shape_kind::sphere:
			out.sphere = { volume.center, volume.radius };
			out.bounds = ds::bounds_of(out.sphere);
			break;
		case scn::shape_kind::capsule:
			out.capsule = { volume.center, volume.end, volume.radius };
			out.bounds = ds::bounds_of(out.capsule);
			break;
		case scn::shape_kind::box:
			out.box = { volume.center, volume.half_extents, glm::mat3_cast(glm::normalize(volume.rotation)) };
			out.bounds = ds::bounds_of(out.box);
			break;
		}
		return out;
	}

	bool shape_meets_triangle(const prepared_shape& volume, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		switch (volume.kind) {
		case scn::shape_kind::sphere: return ds::sphere_triangle(volume.sphere, a, b, c);
		case scn::shape_kind::capsule: return ds::capsule_triangle(volume.capsule, a, b, c);
		case scn::shape_kind::box: return ds::obb_triangle(volume.box, a, b, c);
		}
		return false;
	}

	// Whether a triangle of the mesh meets the shape: the triangles the shape's box reaches (in the mesh's own space), each through
	// the entity's matrix, so a stretched mesh does not stretch the shape. A mesh answered by boxes counts the box of a triangle.
	bool shape_meets_mesh(const entry& mesh, const prepared_shape& volume)
	{
		const triangle_tree& tree = *mesh.tree;
		bool met = false;
		tree.triangles.query_box(ds::transformed(volume.bounds, mesh.inverse), [&](std::uint32_t, const std::uint32_t& first_index) {
			if (tree.boxes_only) {
				met = true;
				return false;
			}
			glm::vec3 a;
			glm::vec3 b;
			glm::vec3 c;
			if (!read_triangle(tree, first_index, a, b, c)) {
				return true;
			}
			met = shape_meets_triangle(volume, to_world(mesh, a), to_world(mesh, b), to_world(mesh, c));
			return !met;
		});
		return met;
	}

	// The nearest point of the mesh's surface to a world point, if it is within `limit`. The triangle tree is walked in the mesh's
	// own space, where distances are not world distances once the matrix stretches: the walk's limit is the world one times the
	// matrix's Frobenius norm (a world distance is at least a local one over that), and each triangle is measured in the world.
	std::optional<scn::point_hit> nearest_in_mesh(const entry& mesh, const glm::vec3& point, float limit)
	{
		const triangle_tree& tree = *mesh.tree;
		const glm::vec3 local_point = glm::vec3(mesh.inverse * glm::vec4(point, 1.f));
		const glm::mat3 inverse_linear{ mesh.inverse };
		float stretch = 0.f;
		for (int column = 0; column < 3; ++column) {
			stretch += glm::dot(inverse_linear[column], inverse_linear[column]);
		}
		stretch = std::sqrt(stretch);
		const auto local_limit = [&](float world_limit) {
			return std::isfinite(world_limit) ? world_limit * stretch : std::numeric_limits<float>::infinity();
		};
		float best = limit;
		std::optional<scn::point_hit> out;
		const auto consider = [&](float distance, const glm::vec3& at, std::uint32_t triangle, const glm::vec3& fallback_normal) {
			if (!(distance < best || (!out && distance <= best))) {
				return;
			}
			best = distance;
			scn::point_hit hit;
			hit.entity = mesh.entity;
			hit.distance = distance;
			hit.point = at;
			// A point within rounding of the surface has no direction to speak of: the triangle's front normal then.
			constexpr float k_on_surface = 1e-5f;
			hit.normal = distance > k_on_surface ? (point - at) / distance : fallback_normal;
			hit.triangle = triangle;
			out = hit;
		};
		tree.triangles.query_nearest(local_point, local_limit(best), [&](std::uint32_t id, const std::uint32_t& first_index, float) {
			if (tree.boxes_only) {
				const ds::aabb3& box = tree.triangles.at(id).box;
				const glm::vec3 at = to_world(mesh, glm::clamp(local_point, box.min, box.max));
				consider(glm::distance(point, at), at, ~0u, glm::vec3{ 0.f, 1.f, 0.f });
			} else {
				glm::vec3 a;
				glm::vec3 b;
				glm::vec3 c;
				if (read_triangle(tree, first_index, a, b, c)) {
					a = to_world(mesh, a);
					b = to_world(mesh, b);
					c = to_world(mesh, c);
					const glm::vec3 at = ds::closest_point_on_triangle(point, a, b, c);
					// The corners are in the world already: a mirrored matrix turns the triangle inside out.
					glm::vec3 front = glm::cross(b - a, c - a);
					if (mesh.mirrored) {
						front = -front;
					}
					const float front_length = glm::length(front);
					consider(glm::distance(point, at), at, first_index / 3,
						front_length > 0.f ? front / front_length : glm::vec3{ 0.f, 1.f, 0.f });
				}
			}
			return ds::bvh_nearest_action{ local_limit(best), false };
		});
		return out;
	}

	// The box of a box's whole travel: where it starts and where it is after `length` along `unit`.
	ds::aabb3 swept_bounds(const ds::aabb3& box, const glm::vec3& unit, float length)
	{
		ds::aabb3 out = box;
		const glm::vec3 shift = unit * length;
		ds::expand(out, box.min + shift);
		ds::expand(out, box.max + shift);
		return out;
	}

	// The first touch of a sphere or capsule moving along `unit` with one mesh, within `limit`: the triangles the shape's travel
	// reaches (in the mesh's own space), each through the entity's matrix and swept in the world.
	std::optional<scn::shape_hit> cast_in_mesh(const entry& mesh, const prepared_shape& volume, const glm::vec3& unit, float limit)
	{
		const triangle_tree& tree = *mesh.tree;
		if (tree.boxes_only) {
			return std::nullopt;
		}
		// Past this the shape has left the entity's box behind: no touch is left to find.
		const ds::aabb3 entity_box = ds::transformed(tree.box, mesh.world);
		const float entity_reach = glm::distance(ds::center(volume.bounds), ds::center(entity_box)) + 0.5f * glm::length(ds::size(entity_box))
			+ 0.5f * glm::length(ds::size(volume.bounds));
		float best = std::min(limit, entity_reach);
		std::optional<scn::shape_hit> found;
		tree.triangles.query_box(ds::transformed(swept_bounds(volume.bounds, unit, best), mesh.inverse),
			[&](std::uint32_t, const std::uint32_t& first_index) {
				glm::vec3 a;
				glm::vec3 b;
				glm::vec3 c;
				if (!read_triangle(tree, first_index, a, b, c)) {
					return true;
				}
				a = to_world(mesh, a);
				b = to_world(mesh, b);
				c = to_world(mesh, c);
				const std::optional<ds::sweep_hit> hit = volume.kind == scn::shape_kind::sphere
					? ds::sphere_sweep_triangle(volume.sphere, unit, a, b, c, best)
					: ds::capsule_sweep_triangle(volume.capsule, unit, a, b, c, best);
				if (hit && (hit->t < best || !found)) {
					best = hit->t;
					scn::shape_hit out;
					out.entity = mesh.entity;
					out.distance = hit->t;
					out.point = hit->point;
					out.normal = hit->normal;
					out.triangle = first_index / 3;
					out.started_inside = hit->started_inside;
					found = out;
				}
				return true;
			});
		return found;
	}

	// Walks the world tree for one ray. `test(index, t_max)` answers the nearest hit inside entry `index`; `found(hit)` returns the
	// distance the walk goes on to (infinity: every entity; the hit's own distance: only nearer ones). For `any` the walk stops at
	// the first hit.
	template <class Test, class Found>
	void walk_world(const ds::bvh<std::uint32_t>& tree, const ds::ray3& ray, float max_distance, mode how, Test&& test, Found&& found)
	{
		float t_max = max_distance;
		bool stopped = false;
		tree.traverse_ray(ray, t_max, [&](std::uint32_t, const std::uint32_t& index, float) {
			if (stopped) {
				return ds::bvh_ray_action{ t_max, true };
			}
			const std::optional<scn::ray_hit> hit = test(index, t_max);
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

	// What an asynchronous batch reads: a copy of the world tree and of every entry, with the flags the registry had when it was
	// taken. The triangle trees are shared and never change after they are built. Nothing in it points into the registry.
	struct snapshot_entry
	{
		entry mesh;
		bool valid = true;
		bool renderable = true;
		scn::spatial_mask layers{ scn::k_default_spatial_layer };
	};

	struct snapshot
	{
		ds::bvh<std::uint32_t> world;
		std::vector<snapshot_entry> entries;
	};

	scn::ray_result run_request(const snapshot& state, const scn::ray_request& request)
	{
		scn::ray_result result;
		const float length = glm::length(request.line.direction);
		if (!(length > 0.f) || !(request.max_distance > 0.f)) {
			return result;
		}
		const scn::ray_filter filter{ request.max_distance, request.mask, request.include_hidden, request.cull_back_faces,
			request.triangles, std::span<const entt::entity>{ request.exclude } };
		const ds::ray3 ray(request.line.origin, request.line.direction / length);
		const mode how = request.kind == scn::ray_query::any ? mode::any : mode::closest;
		const auto test = [&](std::uint32_t index, float t_max) -> std::optional<scn::ray_hit> {
			const snapshot_entry& each = state.entries[index];
			if (!each.valid) {
				return std::nullopt;
			}
			return test_mesh(each.mesh, each.renderable, each.layers, ray, filter, t_max, how);
		};
		if (request.kind == scn::ray_query::all) {
			walk_world(state.world, ray, request.max_distance, how, test, [&result](const scn::ray_hit& hit) {
				result.hits.push_back(hit);
				return std::numeric_limits<float>::infinity();
			});
			std::stable_sort(result.hits.begin(), result.hits.end(), [](const scn::ray_hit& a, const scn::ray_hit& b) {
				return a.distance != b.distance ? a.distance < b.distance : a.entity < b.entity;
			});
			if (request.max_hits != 0 && result.hits.size() > request.max_hits) {
				result.hits.resize(request.max_hits);
				result.truncated = true;
			}
			return result;
		}
		walk_world(state.world, ray, request.max_distance, how, test, [&result](const scn::ray_hit& hit) {
			result.hits.assign(1, hit);
			return hit.distance;
		});
		return result;
	}

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

		void overlap_frustum(entt::registry& world, const ds::frustum& view, const scn::ray_filter& filter, std::vector<entt::entity>& out) override
		{
			ensure_current(world);
			const std::size_t first = out.size();
			m_world.query_frustum(view, [&](std::uint32_t, const std::uint32_t& index, bool) {
				const entry& mesh = m_entries[index];
				if (!world.valid(mesh.entity)) {
					return true;
				}
				const auto* layers = world.try_get<scn::spatial_layers>(mesh.entity);
				if (passes_filter(mesh, world.all_of<scn::renderable>(mesh.entity),
					layers ? layers->mask : scn::spatial_mask{ scn::k_default_spatial_layer }, filter)) {
					out.push_back(mesh.entity);
				}
				return true;
			});
			std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
		}

		void overlap_shape(entt::registry& world, const scn::shape& volume, const scn::ray_filter& filter, std::vector<entt::entity>& out) override
		{
			ensure_current(world);
			const prepared_shape prepared = prepare(volume);
			const std::size_t first = out.size();
			m_world.query_box(prepared.bounds, [&](std::uint32_t, const std::uint32_t& index) {
				const entry& mesh = m_entries[index];
				if (sees(world, mesh, filter) && shape_meets_mesh(mesh, prepared)) {
					out.push_back(mesh.entity);
				}
				return true;
			});
			std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
		}

		void overlap_point(entt::registry& world, const glm::vec3& point, const scn::ray_filter& filter, std::vector<entt::entity>& out) override
		{
			ensure_current(world);
			const std::size_t first = out.size();
			m_world.query_box(ds::aabb3{ point, point }, [&](std::uint32_t, const std::uint32_t& index) {
				const entry& mesh = m_entries[index];
				if (sees(world, mesh, filter)) {
					out.push_back(mesh.entity);
				}
				return true;
			});
			std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
		}

		std::optional<scn::point_hit> closest_point(entt::registry& world, const glm::vec3& point, const scn::ray_filter& filter) override
		{
			if (!(filter.max_distance > 0.f)) {
				return std::nullopt;
			}
			ensure_current(world);
			std::optional<scn::point_hit> best;
			float limit = filter.max_distance;
			m_world.query_nearest(point, limit, [&](std::uint32_t, const std::uint32_t& index, float) {
				const entry& mesh = m_entries[index];
				if (sees(world, mesh, filter)) {
					const auto hit = nearest_in_mesh(mesh, point, limit);
					if (hit && (!best || hit->distance < best->distance || (hit->distance == best->distance && hit->entity < best->entity))) {
						best = hit;
						limit = hit->distance;
					}
				}
				return ds::bvh_nearest_action{ limit, false };
			});
			return best;
		}

		std::optional<scn::shape_hit> shapecast(entt::registry& world, const scn::shape& volume, const glm::vec3& direction,
			const scn::ray_filter& filter) override
		{
			if (volume.kind == scn::shape_kind::box || !(filter.max_distance > 0.f)) {
				return std::nullopt;
			}
			ensure_current(world);
			const ds::aabb3 everything = m_world.bounds();
			if (everything.empty()) {
				return std::nullopt;
			}
			const glm::vec3 unit = glm::normalize(direction);
			const prepared_shape prepared = prepare(volume);
			// An unbounded cast only needs to go as far as the world reaches.
			float limit = filter.max_distance;
			if (!std::isfinite(limit)) {
				limit = glm::length(ds::size(everything)) + glm::distance(ds::center(prepared.bounds), ds::center(everything))
					+ glm::length(ds::size(prepared.bounds));
			}
			std::optional<scn::shape_hit> best;
			m_world.query_box(swept_bounds(prepared.bounds, unit, limit), [&](std::uint32_t, const std::uint32_t& index) {
				const entry& mesh = m_entries[index];
				if (!sees(world, mesh, filter)) {
					return true;
				}
				const auto hit = cast_in_mesh(mesh, prepared, unit, limit);
				if (hit && (!best || hit->distance < best->distance || (hit->distance == best->distance && hit->entity < best->entity))) {
					best = hit;
					limit = hit->distance;
				}
				return true;
			});
			return best;
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
				if (!moving.set_world(transform->world)) {
					egLOG_WARN("scene/spatial", "entity {} has a NaN or infinite world matrix: ray queries skip it", static_cast<std::uint32_t>(entity));
				}
				m_world.set_box(found->second, moving.world_box());
				++refit;
			}
			if (static_cast<float>(refit) > k_rebuild_moved_fraction * static_cast<float>(m_entries.size())
				|| m_world.degradation() > k_rebuild_degradation) {
				m_world.rebuild();
			}
			if (refit != 0) {
				++m_stamp;
			}
		}

		void structure_changed() override
		{
			m_dirty = true;
			++m_stamp;
		}

		void state_changed() override { ++m_stamp; }

		ds::task_handle<scn::ray_batch> raycast_batch(ds::task_system& tasks, entt::registry& world, std::vector<scn::ray_request> requests,
			std::uint64_t frame) override
		{
			ensure_current(world);
			if (!m_snapshot || m_snapshot_stamp != m_stamp) {
				take_snapshot(world);
			}
			// std::function wants a copyable body: the requests are shared, the snapshot is read-only.
			auto asked = std::make_shared<const std::vector<scn::ray_request>>(std::move(requests));
			std::shared_ptr<const snapshot> state = m_snapshot;
			return tasks.run(ds::lane::compute, ds::task_desc{ .name = "spatial.raycast_batch", .category = "spatial" },
				[state, asked, frame](ds::task_context&) {
					scn::ray_batch batch;
					batch.frame = frame;
					batch.results.reserve(asked->size());
					for (const scn::ray_request& request : *asked) {
						batch.results.push_back(run_request(*state, request));
					}
					return batch;
				});
		}

	private:
		// Whether a query with this filter looks at the entry at all: the entity is alive and passes the filter as it is now.
		bool sees(entt::registry& world, const entry& mesh, const scn::ray_filter& filter) const
		{
			if (!world.valid(mesh.entity)) {
				return false;
			}
			const auto* layers = world.try_get<scn::spatial_layers>(mesh.entity);
			return passes_filter(mesh, world.all_of<scn::renderable>(mesh.entity),
				layers ? layers->mask : scn::spatial_mask{ scn::k_default_spatial_layer }, filter);
		}

		// A query against the live world: the registry says what each entity is now.
		template <class Found>
		void walk(entt::registry& world, const scn::ray& line, const scn::ray_filter& filter, mode how, Found&& found)
		{
			const float length = glm::length(line.direction);
			if (!(length > 0.f) || !(filter.max_distance > 0.f)) {
				return;
			}
			ensure_current(world);
			const ds::ray3 ray(line.origin, line.direction / length);
			walk_world(m_world, ray, filter.max_distance, how,
				[&](std::uint32_t index, float t_max) -> std::optional<scn::ray_hit> {
					const entry& mesh = m_entries[index];
					if (!world.valid(mesh.entity)) {
						return std::nullopt;
					}
					const auto* layers = world.try_get<scn::spatial_layers>(mesh.entity);
					return test_mesh(mesh, world.all_of<scn::renderable>(mesh.entity),
						layers ? layers->mask : scn::spatial_mask{ scn::k_default_spatial_layer }, ray, filter, t_max, how);
				},
				found);
		}

		// Builds the trees again when a mesh or geometry component came, went or changed (structure_changed), when the world has
		// another number of meshes than the last build saw, when the geometry holds moved (a hold made, replaced, let go or
		// reloaded), or when a geometry that was loading is ready now. Before the traversal: a walk never sees a half-stale tree.
		void ensure_current(entt::registry& world)
		{
			const std::size_t meshes = world.view<const scn::mesh_component, const scn::world_transform>().size_hint();
			const auto* holds = world.ctx().find<scn::geometry_holds>();
			const std::uint64_t holds_revision = holds ? holds->revision() : 0;
			bool rebuild = !m_built || m_dirty || meshes != m_mesh_count || holds_revision != m_holds_revision;
			if (!rebuild && !m_pending.empty()) {
				rebuild = holds && std::ranges::any_of(m_pending, [&](entt::entity entity) {
					const auto* held = holds->find(entity);
					return held && held->is_ready();
				});
			}
			if (rebuild) {
				build(world);
			}
		}

		void take_snapshot(entt::registry& world)
		{
			auto made = std::make_shared<snapshot>();
			made->world = m_world;
			made->entries.reserve(m_entries.size());
			for (const entry& mesh : m_entries) {
				snapshot_entry copy;
				copy.mesh = mesh;
				copy.valid = world.valid(mesh.entity);
				if (copy.valid) {
					copy.renderable = world.all_of<scn::renderable>(mesh.entity);
					const auto* layers = world.try_get<scn::spatial_layers>(mesh.entity);
					if (layers) {
						copy.layers = layers->mask;
					}
				}
				made->entries.push_back(std::move(copy));
			}
			m_snapshot = std::move(made);
			m_snapshot_stamp = m_stamp;
		}

		void build(entt::registry& world)
		{
			const auto* holds = world.ctx().find<scn::geometry_holds>();
			m_holds_revision = holds ? holds->revision() : 0;
			std::vector<entry> previous = std::move(m_entries);
			std::unordered_map<entt::entity, std::uint32_t> previous_index = std::move(m_index_of);
			m_entries.clear();
			m_index_of.clear();
			m_pending.clear();
			std::vector<ds::bvh<std::uint32_t>::item> items;
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
				if (!mesh.set_world(view.get<const scn::world_transform>(entity).world)) {
					egLOG_WARN("scene/spatial", "entity {} has a NaN or infinite world matrix: ray queries skip it", static_cast<std::uint32_t>(entity));
				}
				if (!held->is_ready()) {
					m_pending.push_back(entity);
					// A geometry that is loading again keeps its old tree until the new block is ready, as the renderer keeps drawing the old one.
					if (const auto before = previous_index.find(entity); before != previous_index.end()) {
						const entry& old = previous[before->second];
						if (old.tree && old.view.vx_begin == range.vx_begin && old.view.ind_begin == range.ind_begin
							&& old.view.ind_end == range.ind_end) {
							mesh.tree = old.tree;
							mesh.key = old.key;
							mesh.geometry = old.geometry;
							used.emplace(old.key, old.tree);
						}
					}
				} else {
					std::shared_ptr<rnd::geometry_desc> geometry = held->get();
					mesh.geometry = geometry.get();
					const tree_key key{ mesh.geometry, range.vx_begin, range.ind_begin, range.ind_end };
					mesh.key = key;
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
			++m_stamp;
		}

		bool m_built = false;
		bool m_dirty = false;
		std::uint64_t m_holds_revision = 0;
		// Moves with everything a snapshot copies; a snapshot is taken anew when it differs from the snapshot's.
		std::uint64_t m_stamp = 1;
		std::uint64_t m_snapshot_stamp = 0;
		std::shared_ptr<const snapshot> m_snapshot;
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
