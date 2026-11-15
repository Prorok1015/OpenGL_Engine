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
		// Faces left out because they have no area.
		std::vector<face_id> skipped_faces;
	};

	// Pure and deterministic: one mesh and settings give the same bytes. Layout is always position vec3, normal vec3, uv vec2,
	// tangent vec3, bitangent vec3 in that order (a shader input is the element's place in the layout), `bounds` stay empty.
	baked_mesh bake(const poly_mesh& mesh, const bake_settings& settings = {});
}
