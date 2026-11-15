#pragma once
#include "edt_mesh_model.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace edt::mesh
{
	inline constexpr std::uint32_t max_shape_segments = 256;

	struct box_params
	{
		glm::vec3 size{ 1.f };
	};

	// In the XZ plane, facing +Y.
	struct plane_params
	{
		glm::vec2 size{ 1.f };
		std::uint32_t segments_x = 1;
		std::uint32_t segments_z = 1;
	};

	struct sphere_params
	{
		float radius = 0.5f;
		std::uint32_t segments = 16;
		std::uint32_t rings = 8;
	};

	struct cylinder_params
	{
		float radius = 0.5f;
		float height = 1.f;
		std::uint32_t segments = 16;
	};

	// `height` is the length of the straight part between the two hemispheres.
	struct capsule_params
	{
		float radius = 0.5f;
		float height = 1.f;
		std::uint32_t segments = 16;
		std::uint32_t hemisphere_rings = 4;
	};

	using shape_params = std::variant<box_params, plane_params, sphere_params, cylinder_params, capsule_params>;

	struct shape_result
	{
		std::optional<poly_mesh> mesh;
		std::string error;
	};

	// Faces are quads (triangles at poles, n-gons on caps), wound counter-clockwise seen from outside, every corner UV in [0, 1].
	// A parameter out of range gives no mesh and an error naming it.
	shape_result make_shape(const shape_params& params);

	std::string_view shape_kind_name(const shape_params& params);
	// The default parameters of a kind ("box", "plane", ...); nullopt for an unknown name.
	std::optional<shape_params> default_shape_params(std::string_view kind);
}
