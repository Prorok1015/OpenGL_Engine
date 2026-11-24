#pragma once
#include <snk/scene/scn_spatial.h>
#include <memory>

namespace scn::detail {
	// The engine's backend: the triangles of the geometry the world's meshes hold (scn_geometry_holds.h).
	std::unique_ptr<spatial_backend> make_mesh_backend();
}
