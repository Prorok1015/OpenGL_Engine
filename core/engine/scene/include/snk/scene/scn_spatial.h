#pragma once
#include <snk/common/ds/ds_bit_frags.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

// Spatial queries of a world (EPIC-81, #1326): a ray against what the world holds, answered by the engine's mesh backend
// (the triangles of the loaded geometry, a BVH per geometry and one over the entities). A module cannot add a backend.
//
// Limits (also in docs/epics/spatial/EPIC-81.md):
//  - a skinned mesh is tested in its bind pose, the vertices as the geometry holds them;
//  - only geometry that is loaded; a mesh whose geometry is not ready is not hit until it is;
//  - triangles only: no colliders, no physics shapes;
//  - a transform is seen once the transform system has run it (it tells the world which entities it moved): a matrix
//    written after that system, or by anything but a transform_updated event, is seen by the next frame's queries at best;
//  - hits are per entity for raycast_all (the nearest of each), per triangle inside an entity for the closest and any.
//  - shape casts (#1328) move a sphere or a capsule (exactly) along a direction; a box is not cast yet (cast_status::unsupported);
//  - shape overlaps (#1328) test the triangles (a mesh of over a million triangles answers by their boxes); overlap_point tests the
//    entities' world bounds only, closest_point the surface: "inside a closed mesh" is not answered (meshes need not be closed);
//  - the frustum query (#1327) tests the entities' world bounds only (the mesh's box through its matrix), not their triangles: an object whose
//    box touches the frustum is in.
namespace scn {

	// The index (0..31) of a layer an entity can be on and a query can ask for. Project names for the layers are a later story.
	enum class spatial_layer : std::uint32_t {};
	using spatial_mask = ds::bit_flags<spatial_layer>;
	inline constexpr spatial_layer k_default_spatial_layer{ 0 };

	// The layers an entity is on, as an engine component. An entity without it is on the default layer (0).
	struct spatial_layers
	{
		spatial_mask mask{ k_default_spatial_layer };
	};

	struct ray
	{
		glm::vec3 origin{ 0.f };
		// Need not be a unit vector: the queries normalize it, and distances are in world units.
		glm::vec3 direction{ 0.f, 0.f, -1.f };
	};

	struct ray_filter
	{
		float max_distance = std::numeric_limits<float>::infinity();
		// Hits entities whose layers share a bit with the mask.
		spatial_mask mask = spatial_mask::all();
		// Entities without `renderable` are skipped unless this is set.
		bool include_hidden = false;
		// A triangle seen from behind is not a hit.
		bool cull_back_faces = false;
		// false: stop at the box of the triangle (cheaper, coarser); `triangle` and `barycentric` of the hit stay empty.
		bool triangles = true;
		// The query passes through these entities (and not through what is below them in the hierarchy). A view of the
		// caller's memory: valid for the call, never stored (an asynchronous batch copies it).
		std::span<const entt::entity> exclude{};
	};

	struct ray_hit
	{
		entt::entity entity = entt::null;
		// Along the normalized ray.
		float distance = 0.f;
		glm::vec3 point{ 0.f };
		// World space, unit length, on the side the ray came from; `back_face` says that side is the triangle's back.
		glm::vec3 normal{ 0.f };
		std::uint32_t triangle = ~0u;
		// point = (1 - x - y) * a + x * b + y * c over the triangle's three vertices.
		glm::vec2 barycentric{ 0.f };
		bool back_face = false;
	};

	enum class ray_query : std::uint8_t
	{
		// The nearest hit: no hit or one.
		closest,
		// Whether anything is hit: no hit or one (the first the walk found, not the nearest).
		any,
		// The nearest hit of every entity the ray meets, nearest first, at most `max_hits` of them.
		all,
	};

	// One ray of an asynchronous batch. Owns its exclude list: the batch outlives the caller's memory.
	struct ray_request
	{
		ray line;
		ray_query kind = ray_query::closest;
		float max_distance = std::numeric_limits<float>::infinity();
		spatial_mask mask = spatial_mask::all();
		bool include_hidden = false;
		bool cull_back_faces = false;
		bool triangles = true;
		std::uint32_t max_hits = 64;
		std::vector<entt::entity> exclude;
	};

	struct ray_result
	{
		std::vector<ray_hit> hits;
		// `all` found more entities than `max_hits`: these are the nearest ones.
		bool truncated = false;
	};

	// The answers of a batch, in the order of its requests, from the world as it was when the batch was handed in.
	struct ray_batch
	{
		// The `frame` the caller gave: the frame whose state these answers describe, however late they arrive.
		std::uint64_t frame = 0;
		std::vector<ray_result> results;
	};

	enum class shape_kind : std::uint32_t
	{
		sphere,
		capsule,
		box,
	};

	// A volume in world space for overlap_shape. Make one with make_sphere(), make_capsule() or make_box(); the other fields are
	// what the shape's kind reads (the rest is ignored).
	struct shape
	{
		shape_kind kind = shape_kind::sphere;
		// sphere and box: the centre; capsule: one end of its axis.
		glm::vec3 center{ 0.f };
		// capsule: the other end of its axis.
		glm::vec3 end{ 0.f };
		// sphere and capsule; not below 0.
		float radius = 0.5f;
		// box: half its size along each of its own axes; not below 0.
		glm::vec3 half_extents{ 0.5f };
		// box: its orientation. Need not be a unit quaternion: the queries normalize it.
		glm::quat rotation{ 1.f, 0.f, 0.f, 0.f };

		static shape make_sphere(const glm::vec3& at, float radius_of)
		{
			shape out;
			out.kind = shape_kind::sphere;
			out.center = at;
			out.radius = radius_of;
			return out;
		}

		static shape make_capsule(const glm::vec3& from, const glm::vec3& to, float radius_of)
		{
			shape out;
			out.kind = shape_kind::capsule;
			out.center = from;
			out.end = to;
			out.radius = radius_of;
			return out;
		}

		static shape make_box(const glm::vec3& at, const glm::vec3& half, const glm::quat& turn = glm::quat{ 1.f, 0.f, 0.f, 0.f })
		{
			shape out;
			out.kind = shape_kind::box;
			out.center = at;
			out.half_extents = half;
			out.rotation = turn;
			return out;
		}
	};

	// The point of an entity's surface nearest to a point.
	struct point_hit
	{
		entt::entity entity = entt::null;
		// From the query's point to `point`, in world units.
		float distance = 0.f;
		glm::vec3 point{ 0.f };
		// Unit length: from `point` towards the query's point, or the triangle's front normal when the query's point is on the surface
		// (within 1e-5; up for a mesh answered by its boxes).
		glm::vec3 normal{ 0.f };
		// The triangle `point` lies on; ~0u for a mesh answered by its boxes.
		std::uint32_t triangle = ~0u;
	};

	// What a shape cast met first.
	struct shape_hit
	{
		entt::entity entity = entt::null;
		// How far the shape moved before it touched, in world units; 0 when it was touching where it started.
		float distance = 0.f;
		// The point of the entity's surface it touches.
		glm::vec3 point{ 0.f };
		// Unit length: from `point` towards the shape at that moment (the way out when it started touching, the triangle's side
		// against the motion when its axis lies on the triangle).
		glm::vec3 normal{ 0.f };
		// The triangle it touches.
		std::uint32_t triangle = ~0u;
		// The shape was touching the entity before it moved (the cast is only told so: distance is 0).
		bool started_inside = false;
	};

	// How a shape cast ended: the path is clear, it met something, the engine does not cast that yet, or the arguments cannot be cast.
	// A value is stored by modules: add new ones only at the end.
	enum class cast_status : std::uint8_t
	{
		// Nothing within the cast's length (also: the world holds nothing to meet).
		clear,
		// `hit` says what it met.
		hit,
		// The shape's kind is not cast (a box, for now): nothing was tested, so the path is not known to be clear.
		unsupported,
		// A shape with a NaN or infinite value or a negative size, a direction with none, or a length that is NaN.
		invalid,
	};

	struct shapecast_result
	{
		cast_status status = cast_status::clear;
		// Meaningful only for cast_status::hit.
		shape_hit hit;
	};

	static_assert(std::is_trivially_copyable_v<shape> && std::is_standard_layout_v<shape>);
	static_assert(std::is_trivially_copyable_v<shape_hit> && std::is_standard_layout_v<shape_hit>);
	static_assert(std::is_trivially_copyable_v<shapecast_result> && std::is_standard_layout_v<shapecast_result>);
	static_assert(std::is_trivially_copyable_v<point_hit> && std::is_standard_layout_v<point_hit>);
	static_assert(std::is_trivially_copyable_v<spatial_layers> && std::is_standard_layout_v<spatial_layers>);
	static_assert(std::is_trivially_copyable_v<ray> && std::is_standard_layout_v<ray>);
	static_assert(std::is_trivially_copyable_v<ray_hit> && std::is_standard_layout_v<ray_hit>);
	static_assert(std::is_trivially_copyable_v<ray_filter> && std::is_standard_layout_v<ray_filter>);

	namespace detail { struct spatial_backend; }

	// The spatial queries of one world, in its registry's ctx. Made by the engine when the world is made (attach); the queries
	// are the free functions below. Main thread, between the frame's systems and the next: not thread safe.
	class SNK_API spatial_world
	{
	public:
		// Made on first use, with the engine's mesh backend (which reads the geometry the world's geometry_holds keeps).
		static spatial_world& attach(entt::registry& world);
		static spatial_world* find(entt::registry& world);

		spatial_world();
		~spatial_world();
		spatial_world(const spatial_world&) = delete;
		spatial_world& operator=(const spatial_world&) = delete;

		std::optional<ray_hit> raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter);
		bool raycast_any(entt::registry& world, const ray& line, const ray_filter& filter);
		// One hit per entity, nearest first (equal distances: by entity).
		void raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out);
		// A callback of the handle (then_main) is not tied to a ds::lifetime owner here: a module that holds the handle must drop it
		// (and any continuation it set) before the module is unloaded.
		// Runs the requests on the task system's compute lane against a snapshot of the world taken now (one copy per state of the
		// world: batches handed in while nothing changed share it). Later moves, creations and destructions do not touch it,
		// and nothing of the batch lives in the frame allocator. The result is read with handle.get() once handle.status() is ready.
		ds::task_handle<ray_batch> raycast_batch(ds::task_system& tasks, entt::registry& world, std::vector<ray_request> requests,
			std::uint64_t frame);
		// The entities whose world bounds meet the frustum of `view_projection` (OpenGL clip space, as glm::perspective builds it;
		// a screen rectangle's frustum comes from screen_rect_view_projection), appended to `out` in entity order. Only the bounds
		// are tested (the mesh's box through the entity's world matrix): an object whose box touches the frustum is in, even when its
		// triangles do not. The test is the usual conservative one (the box's corner furthest along each plane's normal): it may also
		// report a box that lies just outside an edge or a corner of the frustum, never miss one that touches it. The filter's mask,
		// include_hidden and exclude apply as for a ray; max_distance, triangles and cull_back_faces do not. A matrix that gives no
		// frustum (not finite, or one whose four side planes have no normal, such as the all-zero matrix) answers nothing.
		void overlap_frustum(entt::registry& world, const glm::mat4& view_projection, const ray_filter& filter, std::vector<entt::entity>& out);
		// The entities with a triangle that meets `volume` (touching counts), appended to `out` in entity order. The filter's mask,
		// include_hidden and exclude apply as for a ray; max_distance, triangles and cull_back_faces do not. A shape with a NaN or
		// infinite value, a negative size, or a box whose rotation is zero, answers nothing.
		void overlap_shape(entt::registry& world, const shape& volume, const ray_filter& filter, std::vector<entt::entity>& out);
		// The entities whose world bounds contain `point` (the mesh's box through the entity's matrix: conservative, as the frustum
		// query is), appended to `out` in entity order. The filter as for overlap_shape.
		void overlap_point(entt::registry& world, const glm::vec3& point, const ray_filter& filter, std::vector<entt::entity>& out);
		// The nearest point of any entity's surface within `filter.max_distance` (a ray filter's own field: the search radius), or
		// nothing (a max_distance of 0 or less answers nothing). Equal distances: the lower entity. The rest of the filter as for
		// overlap_shape.
		std::optional<point_hit> closest_point(entt::registry& world, const glm::vec3& point, const ray_filter& filter);
		// The first entity `volume` touches moving along `direction` (any length: only its direction counts), within
		// `filter.max_distance` (the length of the cast in world units; unbounded by default; 0 or less: only what the shape already
		// touches). A sphere and a capsule are cast exactly against the triangles (a capsule through its two end caps and through its
		// side against corners and edges). A shape already touching an entity (touching counts) is a hit at distance 0 with
		// `started_inside`. A mesh of over a million triangles is cast against the box of its triangles (the nearest the shape can
		// get to it, `triangle` ~0u); a shape starting inside that box is a hit at 0 (its `point` is the shape's centre, the capsule's first end for a capsule). Equal distances: the lower entity. The filter's
		// mask, include_hidden and exclude apply; `triangles` and `cull_back_faces` do not. A box is not cast yet (cast_status::unsupported).
		shapecast_result shapecast(entt::registry& world, const shape& volume, const glm::vec3& direction, const ray_filter& filter);
		// What the transform system calls for the entities it moved.
		void entities_moved(entt::registry& world, std::span<const entt::entity> moved);

	private:
		static void on_structure_changed(entt::registry& world, entt::entity entity);
		static void on_state_changed(entt::registry& world, entt::entity entity);

		std::unique_ptr<detail::spatial_backend> m_backend;
	};

	// The queries of a world's registry; a world without a spatial_world answers "nothing". They make it on first use.
	SNK_API std::optional<ray_hit> raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter = {});
	SNK_API bool raycast_any(entt::registry& world, const ray& line, const ray_filter& filter = {});
	SNK_API void raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out);

	SNK_API ds::task_handle<ray_batch> raycast_batch(ds::task_system& tasks, entt::registry& world, std::vector<ray_request> requests,
		std::uint64_t frame);

	// The entities whose world bounds meet the frustum of `view_projection` (#1327; see spatial_world::overlap_frustum), in entity order.
	SNK_API void overlap_frustum(entt::registry& world, const glm::mat4& view_projection, const ray_filter& filter, std::vector<entt::entity>& out);
	// The shape and point queries of a world's registry (#1328; see spatial_world).
	SNK_API void overlap_shape(entt::registry& world, const shape& volume, const ray_filter& filter, std::vector<entt::entity>& out);
	SNK_API void overlap_point(entt::registry& world, const glm::vec3& point, const ray_filter& filter, std::vector<entt::entity>& out);
	SNK_API std::optional<point_hit> closest_point(entt::registry& world, const glm::vec3& point, const ray_filter& filter = {});
	SNK_API shapecast_result shapecast(entt::registry& world, const shape& volume, const glm::vec3& direction, const ray_filter& filter = {});
	// The view-projection of the part of an image inside a pixel rectangle: what `overlap_frustum` takes for a box on the screen.
	// `view` and `projection` are the camera's, `corner_a` and `corner_b` two opposite corners in pixels (origin top left, y down, any
	// order), `image_size` the image's size. The corners are not clamped to the image (a rectangle that reaches past it asks for a
	// wider frustum; the editor clamps its own). A rectangle with no area, or an empty image, gives the all-zero matrix, which
	// overlap_frustum answers with nothing.
	SNK_API glm::mat4 screen_rect_view_projection(const glm::mat4& view, const glm::mat4& projection, glm::vec2 corner_a, glm::vec2 corner_b,
		glm::vec2 image_size);

	// The ray through a point of an image. `point` is in pixels, origin top left, y down; `image_size` the image's size.
	// With a camera entity of `world` (camera_component + world_transform); nothing when it has none or the size is empty.
	SNK_API std::optional<ray> ray_from_screen(entt::registry& world, entt::entity camera, glm::vec2 point, glm::vec2 image_size);
	// With plain matrices (OpenGL clip space, as glm::perspective builds it).
	SNK_API ray ray_from_screen(const glm::mat4& view, const glm::mat4& projection, glm::vec2 point, glm::vec2 image_size);

}
