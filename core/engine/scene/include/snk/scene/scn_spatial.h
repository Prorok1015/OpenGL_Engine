#pragma once
#include <snk/common/ds/ds_bit_frags.hpp>
#include <snk/common/snk_api.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

// Spatial queries of a world (EPIC-81, #1326): a ray against what the world holds. The world answers through backends
// (spatial_backend): the engine's own is the mesh backend (the triangles of the loaded geometry, a BVH per geometry and
// one over the entities); a physics backend can join it later without a change in the calls.
//
// Limits (also in docs/epics/spatial/EPIC-81.md):
//  - a skinned mesh is tested in its bind pose, the vertices as the geometry holds them;
//  - only geometry that is loaded; a mesh whose geometry is not ready is not hit until it is;
//  - triangles only: no colliders, no physics shapes;
//  - a transform is seen once the transform system has run it (it tells the world which entities it moved): a matrix
//    written after that system, or by anything but a transform_updated event, is seen by the next frame's queries at best;
//  - hits are per entity for raycast_all (the nearest of each), per triangle inside an entity for the closest and any.
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

	static_assert(std::is_trivially_copyable_v<spatial_layers> && std::is_standard_layout_v<spatial_layers>);
	static_assert(std::is_trivially_copyable_v<ray> && std::is_standard_layout_v<ray>);
	static_assert(std::is_trivially_copyable_v<ray_hit> && std::is_standard_layout_v<ray_hit>);
	static_assert(std::is_trivially_copyable_v<ray_filter> && std::is_standard_layout_v<ray_filter>);

	// A module or the engine fills this vtable (spatial_world::add_backend) and the engine calls it: raised with any change of
	// its bases or virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t spatial_backend_abi_revision = 1;

	// Answers the queries of one world from what it knows about it. The mesh backend is the engine's; a physics backend
	// would be another. The registry is handed in by every call, never kept: a registry can be moved.
	struct spatial_backend
	{
		virtual ~spatial_backend() = default;
		// The nearest hit within the filter's distance.
		virtual std::optional<ray_hit> raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter) = 0;
		// Whether anything is hit: stops at the first hit it finds.
		virtual bool raycast_any(entt::registry& world, const ray& line, const ray_filter& filter) = 0;
		// The nearest hit of every entity the ray meets (one per entity), nearest first, appended to `out`.
		virtual void raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out) = 0;
		// The transform system moved these entities (and their descendants) this frame: their world matrices are new.
		virtual void entities_moved(entt::registry& world, std::span<const entt::entity> moved) = 0;
	};

	// The backends of one world, in its registry's ctx. Made by the engine when the world is made (attach); queries from
	// the free functions below. Main thread, between the frame's systems and the next: not thread safe.
	class SNK_API spatial_world
	{
	public:
		// Made on first use with the mesh backend (which reads the geometry the world's geometry_holds keeps).
		static spatial_world& attach(entt::registry& world);
		static spatial_world* find(entt::registry& world);

		spatial_world();
		~spatial_world();
		spatial_world(const spatial_world&) = delete;
		spatial_world& operator=(const spatial_world&) = delete;

		void add_backend(std::unique_ptr<spatial_backend> backend);
		std::size_t backend_count() const;

		// The nearest of every backend's nearest.
		std::optional<ray_hit> raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter);
		bool raycast_any(entt::registry& world, const ray& line, const ray_filter& filter);
		// Every backend's hits merged by distance.
		void raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out);
		// What the transform system calls for the entities it moved.
		void entities_moved(entt::registry& world, std::span<const entt::entity> moved);

	private:
		std::vector<std::unique_ptr<spatial_backend>> m_backends;
	};

	// The queries of a world's registry; a world without a spatial_world answers "nothing". They make it on first use.
	SNK_API std::optional<ray_hit> raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter = {});
	SNK_API bool raycast_any(entt::registry& world, const ray& line, const ray_filter& filter = {});
	SNK_API void raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out);

	// The ray through a point of an image. `point` is in pixels, origin top left, y down; `image_size` the image's size.
	// With a camera entity of `world` (camera_component + world_transform); nothing when it has none or the size is empty.
	SNK_API std::optional<ray> ray_from_screen(entt::registry& world, entt::entity camera, glm::vec2 point, glm::vec2 image_size);
	// With plain matrices (OpenGL clip space, as glm::perspective builds it).
	SNK_API ray ray_from_screen(const glm::mat4& view, const glm::mat4& projection, glm::vec2 point, glm::vec2 image_size);

}
