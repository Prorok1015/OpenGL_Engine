#pragma once
#include "edt_mesh_model.h"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <cstdint>
#include <vector>

namespace edt::mesh
{
	struct bake_settings
	{
		// Neighbouring faces share a vertex normal while the angle between them is at most this and their edge is not sharp.
		float smooth_angle_degrees = 45.f;
	};

	// The vertices and indices of one material slot inside the baked buffers. Indices count from vx_begin, as a mesh node's
	// range expects (the draw adds the base vertex).
	struct material_range
	{
		std::uint32_t slot = 0;
		std::uint32_t vx_begin = 0;
		std::uint32_t vx_end = 0;
		std::uint32_t ind_begin = 0;
		std::uint32_t ind_end = 0;
	};

	struct baked_mesh
	{
		rnd::geometry_desc geometry;
		std::vector<material_range> ranges;
		// Faces left out, see bake().
		std::vector<face_id> skipped_faces;
	};

	// Pure and deterministic: one mesh and settings give the same bytes from the same binary (float math may differ between
	// compilers, libm and CPUs, so a file written elsewhere can differ in the last bit). Vertices follow the order of the
	// faces and their corners, never the vertex ids. Vertices are split by UV and by sharp edges only: the `seam` flag
	// belongs to the UV tools and is not read here. Faces the bake cannot triangulate (no area, a vertex used twice, a
	// position that is not finite, a polygon that crosses itself, winds twice or is concave with more than 64 corners)
	// are left out and listed in `skipped_faces`. In a convex fan the triangles of zero area (a collinear vertex 0) are dropped and the
	// vertex stays unused by them, so a neighbour that splits that edge can meet it in a T-junction. The thresholds 1e-9 (turn),
	// 1e-12 (area) and 1e-4 (UV) are absolute or relative to the UV edges, not to the size of the mesh: a mesh far from unit scale
	// may lose its thinnest faces. Layout is always position vec3, normal vec3, uv vec2,
	// tangent vec3, bitangent vec3 in that order (a shader input is the element's place in the layout), `bounds` stay empty.
	baked_mesh bake(const poly_mesh& mesh, const bake_settings& settings = {});
}
