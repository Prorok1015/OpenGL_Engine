#pragma once
#include "edt_mesh_picking.h"
#include "edt_mesh_transform.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <glm/glm.hpp>

namespace edt::mesh
{
	enum class gizmo_tool : std::uint8_t
	{
		move,
		rotate,
		scale,
	};

	// What of the gizmo is under the mouse. Move: x/y/z arrows, yz/zx/xy plane handles, centre = in the plane of the screen.
	// Rotate: x/y/z rings, screen = the ring around the view direction. Scale: x/y/z cubes, centre = all three together.
	enum class gizmo_part : std::uint8_t
	{
		none,
		x,
		y,
		z,
		yz,
		zx,
		xy,
		centre,
		screen,
	};

	// Sizes in pixels: the panel gives them in em() so they follow the font and the DPI.
	struct gizmo_metrics
	{
		float arm = 90.f;
		float pick = 7.f;
		float cube = 5.f;
		float centre = 8.f;
		float plane_near = 0.25f;
		float plane_far = 0.5f;
	};

	// An axis or a plane that faces the camera cannot be dragged along (or in), so it fades out and is not picked.
	inline constexpr float k_gizmo_fade_start = 0.9f;
	inline constexpr float k_gizmo_fade_end = 0.99f;

	// One piece of the gizmo as the screen sees it.
	struct gizmo_shape
	{
		gizmo_part part = gizmo_part::none;
		// 1 = drawn in full, 0 = not drawn and not pickable.
		float visibility = 1.f;
		// Points in pixels: an arrow or a cube stalk is two (origin, end), a ring is a closed polyline, a plane handle four corners.
		std::vector<glm::vec2> points;
		// Where the cube of a scale axis sits, or the middle of the centre handle.
		glm::vec2 anchor{ 0.f };
	};

	struct gizmo_layout
	{
		gizmo_tool tool = gizmo_tool::move;
		pivot_frame frame;
		glm::vec2 origin{ 0.f };
		float world_per_pixel = 0.f;
		glm::vec3 view_direction{ 0.f, 0.f, -1.f };
		float screen_ring_radius = 0.f;
		std::vector<gizmo_shape> shapes;

		const gizmo_shape* find(gizmo_part part) const;
	};

	// nullopt when the pivot is behind the camera or the matrix cannot be inverted.
	std::optional<gizmo_layout> make_gizmo_layout(const screen_view& screen, const pivot_frame& frame, gizmo_tool tool, const gizmo_metrics& metrics);

	// The part under `mouse`: of several the one whose handle is nearest the pointer; the centre wins ties. Parts that fade out are skipped.
	gizmo_part pick_gizmo_part(const gizmo_layout& layout, const glm::vec2& mouse, const gizmo_metrics& metrics);

	// Snap steps: 0 = off for that kind.
	struct gizmo_snap
	{
		float move = 0.f;
		float rotate_radians = 0.f;
		float scale = 0.f;
	};

	// What a drag makes of the snapshot: a matrix to apply to it and the numbers to show.
	struct gizmo_delta
	{
		glm::mat4 matrix{ 1.f };
		glm::vec3 translation{ 0.f };
		float angle_radians = 0.f;
		glm::vec3 factors{ 1.f };
	};

	// A drag from the press to the release. All maths are against the state at the press, never against the last frame.
	class gizmo_drag
	{
	public:
		// nullopt when the press cannot be turned into a drag (a ray that misses the plane, a pivot behind the camera).
		static std::optional<gizmo_drag> begin(const screen_view& screen, const gizmo_layout& layout, gizmo_part part, const glm::vec2& mouse);

		// nullopt for a mouse position the drag cannot read (a ray parallel to the plane or the axis): the caller keeps the last delta.
		std::optional<gizmo_delta> update(const screen_view& screen, const glm::vec2& mouse, const gizmo_snap& snap) const;

		gizmo_tool tool() const { return m_tool; }
		gizmo_part part() const { return m_part; }

	private:
		gizmo_tool m_tool = gizmo_tool::move;
		gizmo_part m_part = gizmo_part::none;
		pivot_frame m_frame;
		glm::vec3 m_view_direction{ 0.f, 0.f, -1.f };
		glm::vec2 m_origin_pixel{ 0.f };
		glm::vec2 m_press{ 0.f };
		glm::vec3 m_start_point{ 0.f };
		float m_start_param = 0.f;
	};

	// Exact values typed in the numbers panel, as a matrix over the snapshot: move = the pivot goes to `values` (world), rotate = degrees about
	// the frame's x, y, z in that order, scale = factors along the frame's axes.
	glm::mat4 numeric_matrix(gizmo_tool tool, const pivot_frame& frame, const glm::vec3& values);
	// What the numbers panel starts with: the pivot's position, 0 degrees, factors of 1.
	glm::vec3 numeric_defaults(gizmo_tool tool, const pivot_frame& frame);
}
