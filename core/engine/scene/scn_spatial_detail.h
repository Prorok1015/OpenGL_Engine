#pragma once
#include <snk/common/ds/ds_intersect.hpp>
#include <snk/scene/scn_spatial.h>
#include <entt/fwd.hpp>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace scn::detail {
	// What spatial_world asks of a backend. Private to the scene library: the engine's mesh backend is the only one, and a
	// module cannot add another (#1326; a module backend would be its own story, registered through module_context and
	// removed when the module unloads).
	struct spatial_backend
	{
		virtual ~spatial_backend() = default;
		virtual std::optional<ray_hit> raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter) = 0;
		virtual bool raycast_any(entt::registry& world, const ray& line, const ray_filter& filter) = 0;
		// The nearest hit of every entity the ray meets, nearest first (equal distances by entity), appended to `out`.
		virtual void raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out) = 0;
		virtual ds::task_handle<ray_batch> raycast_batch(ds::task_system& tasks, entt::registry& world, std::vector<ray_request> requests,
			std::uint64_t frame) = 0;
		// The entities whose world bounds meet the frustum and pass the filter's flags, appended to `out` in entity order.
		virtual void overlap_frustum(entt::registry& world, const ds::frustum& view, const ray_filter& filter, std::vector<entt::entity>& out) = 0;
		virtual void overlap_shape(entt::registry& world, const shape& volume, const ray_filter& filter, std::vector<entt::entity>& out) = 0;
		virtual void overlap_point(entt::registry& world, const glm::vec3& point, const ray_filter& filter, std::vector<entt::entity>& out) = 0;
		virtual std::optional<point_hit> closest_point(entt::registry& world, const glm::vec3& point, const ray_filter& filter) = 0;
		// `direction` has unit length; the volume is one `usable` for a cast. A kind the backend does not cast answers unsupported.
		virtual shapecast_result shapecast(entt::registry& world, const shape& volume, const glm::vec3& direction, const ray_filter& filter) = 0;
		// The transform system moved these entities (and their descendants): their world matrices are new.
		virtual void entities_moved(entt::registry& world, std::span<const entt::entity> moved) = 0;
		// A mesh or geometry component was made, changed or destroyed: what the backend built may be wrong.
		virtual void structure_changed() = 0;
		// A flag a snapshot copies (renderable, spatial layers) changed: the trees stay, the next snapshot is taken anew.
		virtual void state_changed() = 0;
	};

	// The engine's backend: the triangles of the geometry the world's meshes hold (scn_geometry_holds.h).
	std::unique_ptr<spatial_backend> make_mesh_backend();
}
