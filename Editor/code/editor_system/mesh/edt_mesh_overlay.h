#pragma once
#include "edt_mesh_adjacency.h"
#include "edt_mesh_model.h"
#include "edt_mesh_picking.h"
#include "edt_mesh_selection.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <glm/glm.hpp>

namespace edt::mesh
{
	// The element the cursor is over; only the one of the selection's mode is looked for and drawn (a vertex in vertex mode, an edge
	// in edge mode, a face in face mode).
	struct hover_target
	{
		std::optional<vertex_id> vertex;
		std::optional<edge_key> edge;
		std::optional<face_id> face;

		bool empty() const { return !vertex && !edge && !face; }
	};

	// Drawn in this order: later over earlier.
	enum class overlay_kind : std::uint8_t
	{
		plain,
		hover,
		selected,
	};

	struct overlay_segment
	{
		glm::vec2 from{ 0.f };
		glm::vec2 to{ 0.f };
		overlay_kind kind = overlay_kind::plain;
	};

	struct overlay_point
	{
		glm::vec2 position{ 0.f };
		overlay_kind kind = overlay_kind::plain;
	};

	// A selected or hovered face as the polygon it covers on the screen.
	struct overlay_polygon
	{
		std::vector<glm::vec2> corners;
		overlay_kind kind = overlay_kind::hover;
	};

	struct overlay_frame
	{
		std::vector<overlay_segment> segments;
		std::vector<overlay_point> points;
		std::vector<overlay_polygon> polygons;
		// Plain edges left out because of the budget: the status line says so.
		std::size_t omitted_segments = 0;
	};

	// 60 000 segments a frame (EPIC-77 6.1).
	inline constexpr std::size_t k_overlay_segment_budget = 60000;

	struct overlay_options
	{
		// Show what faces away from the camera too.
		bool xray = false;
		std::size_t segment_budget = k_overlay_segment_budget;
		// Over the budget the plain edges nearest to the cursor stay; without a cursor the first ones.
		std::optional<glm::vec2> cursor;
	};

	// What to draw over the preview image, in pixels, with no ImGui: the edges (a face turned away from the camera takes its
	// own edges with it, and an edge or a vertex any of whose faces looks at the camera stays), the vertices in vertex mode, the
	// selected and hovered faces as polygons. Selected goes over hovered goes over plain. Past the budget the selected and the
	// hovered segments stay and the plain ones go, the ones farthest from the cursor first (by the distance from the cursor to the segment). A part of an edge behind the camera is
	// cut off; a face with a corner behind the camera has no polygon. Depth is not tested (an edge behind a nearer face shows
	// when its own face looks at the camera): the overlay is a layer over the image, as the design says.
	overlay_frame build_overlay(const poly_mesh& mesh, const mesh_adjacency& adjacency, const mesh_selection& selection,
		const hover_target& hover, const screen_view& view, const overlay_options& options = {});
}
