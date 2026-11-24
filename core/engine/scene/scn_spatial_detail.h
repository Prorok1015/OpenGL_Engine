#pragma once
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
		// The transform system moved these entities (and their descendants): their world matrices are new.
		virtual void entities_moved(entt::registry& world, std::span<const entt::entity> moved) = 0;
		// A mesh or geometry component was made, changed or destroyed: what the backend built may be wrong.
		virtual void structure_changed() = 0;
	};

	// The engine's backend: the triangles of the geometry the world's meshes hold (scn_geometry_holds.h).
	std::unique_ptr<spatial_backend> make_mesh_backend();
}
